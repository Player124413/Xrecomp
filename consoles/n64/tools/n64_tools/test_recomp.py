"""pytest for the N64 static recompiler's Python side (XBA-169 / XBA-170): ROM parsing, the MIPS III decoder,
function discovery on hand-encoded code, PI DMA segments, the lifter's C (compiled against the runtime headers when a
C compiler is there) and the CLI. Runs with the tools suite (pytest tools/ consoles/)."""

import json
import os
import shutil
import struct
import subprocess
import sys

import pytest

from n64_tools import dma, rom
from n64_tools.discover import Discovery, Function, Image, recomp_hash
from n64_tools.isa import decode
from n64_tools.lift import FnLifter
from n64_tools.recomp import main as recomp_main
from n64_tools.recomp import read_discovered

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
RUNTIME = os.path.join(REPO, "consoles", "n64", "runtime")
sys.path.insert(0, os.path.join(REPO, "consoles", "n64", "tests"))


def r_type(funct, rs=0, rt=0, rd=0, sa=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def j_type(op, target):
    return (op << 26) | ((target >> 2) & 0x3FFFFFF)


JR_RA = r_type(0x08, 31)
NOP = 0


def be(words):
    return struct.pack(">%dI" % len(words), *words)


def synthetic_rom():
    import recomp_fuzz
    data, _gen = recomp_fuzz.make_rom(3, 200)
    return data


# ---- ROM ----

def test_byte_orders_and_header():
    z = synthetic_rom()
    v = bytearray(z)
    v[0::2], v[1::2] = z[1::2], z[0::2]
    n = bytearray(z)
    n[0::4], n[1::4], n[2::4], n[3::4] = z[3::4], z[2::4], z[1::4], z[0::4]
    for raw, order in ((z, rom.Z64), (bytes(v), rom.V64), (bytes(n), rom.N64)):
        r = rom.Rom(raw)
        assert r.order == order
        assert r.data == z
        assert r.boot == "classic" and r.entry == 0x80000400
        assert r.segments[0].vaddr == 0x80000400 and r.segments[0].data[:16] == z[0x1000:0x1010]
    assert rom.Rom(z).title == "XBA RCFUZZ"
    with pytest.raises(ValueError):
        rom.Rom(b"\0" * 0x2000)


def test_libdragon_elf_in_rom():
    # an ELF32 big-endian MIPS executable with one PT_LOAD at 0x80000400, at ROM 0x1100 (256-byte boundary)
    code = be([i_type(0x09, 0, 2, 7), JR_RA, NOP])
    ehdr = bytearray(52)
    ehdr[:6] = b"\x7fELF\x01\x02"
    struct.pack_into(">HHIIII", ehdr, 16, 2, 8, 1, 0x80000400, 52, 0)
    struct.pack_into(">HH", ehdr, 42, 32, 1)
    phdr = struct.pack(">8I", 1, 0x100, 0x80000400, 0x80000400, len(code), 0x40, 5, 16)
    img = bytearray(0x3000)
    img[:4] = b"\x80\x37\x12\x40"
    struct.pack_into(">I", img, 8, 0x80000400)
    elf = bytes(ehdr) + phdr
    img[0x1100:0x1100 + len(elf)] = elf
    img[0x1200:0x1200 + len(code)] = code
    r = rom.Rom(bytes(img))
    assert r.boot == "libdragon-elf"
    assert r.entry == 0x80000400
    assert len(r.segments) == 1 and r.segments[0].executable and r.segments[0].data == code


# ---- decoder ----

@pytest.mark.parametrize("word,name,cls", [
    (r_type(0x2D, 1, 2, 3), "daddu", "alu"),
    (r_type(0x3F, 0, 2, 3, 4), "dsra32", "alu"),
    (r_type(0x20, 1, 2, 3), "add", "ovf"),
    (r_type(0x34, 1, 2), "teq", "trap"),
    (r_type(0x1E, 1, 2), "ddiv", "muldiv"),
    (i_type(0x1A, 1, 2, 3), "ldl", "load"),
    (i_type(0x2D, 1, 2, 3), "sdr", "store"),
    (i_type(0x15, 1, 2, 3), "bnel", "branch"),
    (0x45030004, "bc1tl", "branch"),
    (0x44A21000, "dmtc1", "fmove"),
    (0x46221080, "fop", "fop"),
    (i_type(0x35, 1, 2, 8), "ldc1", "fmem"),
    (i_type(0x2F, 1, 0x19, 0), "cache", "cache"),
    (0x42000018, "cop0", "interp"),
    (i_type(0x30, 1, 2, 0), "ll", "interp"),
    (0x7C000000, "reserved", "interp"),
])
def test_decode(word, name, cls):
    i = decode(word, 0x80001000)
    assert (i.name, i.cls) == (name, cls)


def test_decode_targets_and_links():
    j = decode(j_type(0x03, 0x80123450), 0x80100000)
    assert j.target == 0x80123450 and j.link == 31
    b = decode(i_type(0x01, 4, 0x13, -2), 0x80001000)  # BGEZALL
    assert b.target == 0x80001000 + 4 - 8 and b.likely and b.link == 31
    assert set(decode(i_type(0x22, 4, 5, 0), 0).reads()) == {4, 5}  # LWL merges with rt


# ---- discovery ----

def program():
    """main: JAL leaf ; a jump table switch (cases after main's return) ; a pointer in data ; an unreached function
    after padding ; the table after the text."""
    base = 0x80000400
    w = []
    # 0x400 main
    w += [i_type(0x09, 29, 29, -24 & 0xFFFF), r_type(0x25, 31, 0, 16),
          j_type(0x03, 0x80000450), NOP,
          i_type(0x0B, 4, 1, 3),                 # sltiu at, a0, 3
          i_type(0x04, 1, 0, 10),                # beqz at, out
          r_type(0x00, 0, 4, 4, 2),              # sll a0, a0, 2
          i_type(0x0F, 0, 1, 0x8000),            # lui at, 0x8000
          r_type(0x21, 1, 4, 1),                 # addu at, at, a0
          i_type(0x23, 1, 2, 0x0490),            # lw v0, 0x490(at)
          r_type(0x08, 2), NOP,                  # jr v0
          i_type(0x09, 0, 2, 1), JR_RA, NOP, NOP]  # 0x430: case 0 ; return
    # 0x440 case 1 / 2 (in main)
    w += [i_type(0x09, 0, 2, 2), JR_RA, NOP, NOP]
    # 0x450 leaf
    w += [i_type(0x09, 0, 2, 5), JR_RA, NOP, NOP]
    # 0x460 function only reachable through a pointer in data
    w += [i_type(0x09, 29, 29, -8 & 0xFFFF), i_type(0x09, 29, 29, 8), JR_RA, NOP]
    # 0x470 padding, then an unreached function at 0x480
    w += [NOP] * 4
    w += [i_type(0x09, 0, 2, 9), JR_RA, NOP, NOP]
    # 0x490 data after the text: the jump table 0x430, 0x450, 0x450, then a pointer to 0x460
    w += [0x80000430, 0x80000440, 0x80000440, 0x80000460]
    return Image(base, be(w), "elf")


def test_discovery():
    img = program()
    d = Discovery(img, {0x80000400})
    funcs = d.run()
    starts = {f.start for f in funcs}
    assert {0x80000400, 0x80000450, 0x80000460, 0x80000480} <= starts
    assert 0x80000430 not in starts and 0x80000440 not in starts  # case labels stay inside main
    assert d.stats["jump_tables"] == 1
    main = next(f for f in funcs if f.start == 0x80000400)
    assert main.end > 0x80000430
    f = Function(img, 0x80000450, 0x8000045C)
    assert f.hash == recomp_hash(img.words(0x80000450, 0x8000045C))


def test_dma_segments():
    # LUI a0, 0x8030 (dst) ; LUI a1, 0x10 (ROM 0x100000) ; ORI a2, zero, 0x40 (len) ; JAL dma ; NOP
    code = be([i_type(0x0F, 0, 4, 0x8030), i_type(0x0F, 0, 5, 0x0010), i_type(0x0D, 0, 6, 0x40),
               j_type(0x03, 0x80000500), NOP, JR_RA, NOP])
    data = bytearray(0x101000 + 0x40)
    data[:4] = b"\x80\x37\x12\x40"
    struct.pack_into(">I", data, 8, 0x80000400)
    data[0x1000:0x1000 + len(code)] = code
    seg = be([i_type(0x09, 0, 2, 1)] * 15 + [JR_RA])
    data[0x100000:0x100000 + len(seg)] = seg
    r = rom.Rom(bytes(data))
    imgs = [Image(s.vaddr, s.data, s.origin) for s in r.segments]
    found = dma.find_segments(r, imgs)
    assert any(s.base == 0x80300000 and s.data[:len(seg)] == seg for s in found)


# ---- lifter ----

ALL_KINDS = [
    r_type(0x21, 4, 5, 2), r_type(0x2D, 4, 5, 3), r_type(0x3C, 0, 3, 3, 1), r_type(0x07, 6, 3, 3),
    i_type(0x09, 4, 4, -1), i_type(0x19, 4, 4, 8), i_type(0x0F, 0, 8, 0x8010), i_type(0x0B, 4, 9, 7),
    r_type(0x20, 4, 5, 6), i_type(0x18, 4, 7, 3), r_type(0x34, 4, 5), i_type(0x01, 4, 0x0C, 9),
    r_type(0x1C, 4, 5), r_type(0x1B, 4, 5), r_type(0x10, 0, 0, 10), r_type(0x13, 4),
    i_type(0x0F, 0, 9, 0xA440), i_type(0x23, 9, 11, 0x10),
    i_type(0x23, 8, 11, 0), i_type(0x37, 8, 12, 8), i_type(0x1A, 8, 12, 3), i_type(0x26, 8, 11, 5),
    i_type(0x2B, 8, 11, 0), i_type(0x2D, 8, 12, 9), i_type(0x2A, 8, 11, 2), i_type(0x28, 8, 4, 1),
    0x44822000, 0x44A32000, 0x44022000, 0x44CA7800, 0x444A7800, i_type(0x35, 8, 2, 16), i_type(0x39, 8, 3, 4),
    0x46221080, 0x46000106,
    i_type(0x2F, 8, 0x19, 0), 0x0000000F, 0x40024800, 0x42000002, i_type(0x30, 8, 2, 0),
    i_type(0x14, 4, 5, 3), r_type(0x21, 4, 5, 2), NOP, NOP,
    0x45010002, i_type(0x09, 0, 2, 1), NOP,
    i_type(0x01, 4, 0x11, 2), NOP, NOP,
    j_type(0x03, 0x80000400), NOP,
    r_type(0x09, 25, 0, 31), NOP,
    JR_RA, NOP,
]


def lifted_c(comments=True, resume_all=False):
    img = Image(0x80000400, be(ALL_KINDS), "elf")
    f = Function(img, 0x80000400, img.end)
    return FnLifter(f, "n64_rc_test", resume_all=resume_all).function(comments)


def test_lift_shapes():
    c = lifted_c()
    assert "RC_ENTER(N64_RC_CPI_CACHED)" in c
    assert "RC_LOADP_RAM(w_, ea_, 8" in c and "rc_ldl(" in c  # r8 = LUI 0x8010: cached RDRAM, inline
    assert "RC_STOREP_RAM(ea_, RC_SDR_D(" in c
    assert "RC_LOAD(r11, r9 + 0x0000000000000010ULL, 4, RC_S32" in c  # r9 = LUI 0xA440 (VI): through the core
    assert "RC_COP1(0x46221080u" in c
    assert "RC_SEG(0x80000400u" in c and "RC_MID_TABLE(0x80000400u, mid_)" in c  # blocks
    assert "RC_INTERP(0x80000494u)" in c  # MFC0 goes to the interpreter
    assert "if (!tk) { goto L_" in c  # branch-likely skips its slot
    assert "RC_EXIT64(jt);" in c


@pytest.mark.skipif(not shutil.which(os.environ.get("CC", "cc")), reason="no C compiler")
@pytest.mark.parametrize("resume_all", [False, True])
def test_lift_compiles(tmp_path, resume_all):
    src = tmp_path / "t.c"
    src.write_text('#include "n64_recomp_rt.h"\n' + lifted_c(resume_all=resume_all))
    cc = os.environ.get("CC", "cc")
    r = subprocess.run([cc, "-std=c11", "-O1", "-Wall", "-Werror", "-Wno-unused-label", "-Wno-unused-variable",
                        "-Wno-unused-but-set-variable", "-I", RUNTIME, "-c", str(src), "-o", str(tmp_path / "t.o")],
                       capture_output=True, text=True, check=False)
    assert r.returncode == 0, r.stderr


# ---- CLI ----

def test_cli(tmp_path, capsys):
    path = tmp_path / "fuzz.z64"
    path.write_bytes(synthetic_rom())
    disc = tmp_path / "discovered.txt"
    # a recorded function at 0x80000180 (the vector: not in the ROM's image): lifted from the recorded bytes
    words = [i_type(0x09, 0, 2, 3), 0x42000018, NOP]
    disc.write_text("# test\n80000180 %016X %s\n" % (recomp_hash(words), be(words).hex().upper()))
    assert read_discovered(str(disc))[0][0] == 0x80000180
    out = tmp_path / "src"
    assert recomp_main([str(path), str(out), "--discovered", str(disc), "--report", str(tmp_path / "r.json")]) == 0
    summary = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert summary["ram_functions"] >= 1 and summary["functions"] > 5
    tab = (out / "n64_rc_table.c").read_text()
    assert "n64_recomp_title_table" in tab and "0x80000180u" in tab and "0x80000400u" in tab
    names = sorted(p.name for p in out.iterdir())
    # files named after their first function (here the recorded code at 0x80000180, with its hash)
    assert any(n.startswith("n64_rc_80000180_") for n in names)
    # stable: a second run with the same input writes the same files
    first = {n: (out / n).read_text() for n in names}
    assert recomp_main([str(path), str(out), "--discovered", str(disc)]) == 0
    capsys.readouterr()
    assert {p.name: p.read_text() for p in out.iterdir()} == first
