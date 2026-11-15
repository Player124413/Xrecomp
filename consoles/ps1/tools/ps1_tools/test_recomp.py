"""pytest for the PS1 static recompiler's Python half (XBA-160 / XBA-161): decoder, input parsing (PS-X EXE, ISO 9660
on cooked and raw images), discovery on hand-encoded code, the discovered-file round trip with overlays, the CLI.
Runs with the tools suite (pytest tools/ consoles/); the C side is tested by consoles/ps1/tests (ps1_rc_*)."""

import json
import os
import shutil
import struct
import subprocess

import pytest

from ps1_tools import isa
from ps1_tools.discover import Discovery, Image, bios_stub, merge_records, recomp_hash
from ps1_tools.exe import Disc, Exe, load_input
from ps1_tools.recomp import build_functions, main, merge_variants, read_discovered, recompile

RUNTIME = os.path.join(os.path.dirname(__file__), "..", "..", "runtime")


# ---- tiny assembler ----
def r_type(funct, rs=0, rt=0, rd=0, sa=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def jal(t):
    return (3 << 26) | ((t >> 2) & 0x3FFFFFF)


def j(t):
    return (2 << 26) | ((t >> 2) & 0x3FFFFFF)


NOP = 0
JR_RA = r_type(0x08, isa.RA)
ADDIU = lambda rt, rs, imm: i_type(0x09, rs, rt, imm)  # noqa: E731
LW = lambda rt, off, rs: i_type(0x23, rs, rt, off)  # noqa: E731
SW = lambda rt, off, rs: i_type(0x2B, rs, rt, off)  # noqa: E731
LUI = lambda rt, imm: i_type(0x0F, 0, rt, imm)  # noqa: E731


def words(ws):
    return b"".join(struct.pack("<I", w & 0xFFFFFFFF) for w in ws)


def make_exe(text, base=0x80010000, pc0=None):
    text = text + bytes((-len(text)) % 2048)
    head = bytearray(2048)
    head[0:8] = b"PS-X EXE"
    struct.pack_into("<10I", head, 0x10, pc0 or base, 0, base, len(text), 0, 0, 0, 0, 0x801FFF00, 0)
    return bytes(head) + text


# ---- decoder ----
def test_decode_basics():
    i = isa.decode(ADDIU(isa.SP, isa.SP, -24), 0x80010000)
    assert (i.name, i.cls, i.rt, i.rs, i.simm) == ("addiu", "alu", isa.SP, isa.SP, -24)
    assert i.text() == "addiu sp, sp, -24"
    b = isa.decode(i_type(4, 2, 0, 3), 0x80010010)  # beq v0, zero, +3
    assert b.cls == "branch" and b.target == 0x80010020 and b.reads() == {2, 0}
    jl = isa.decode(jal(0x80012340), 0x80010000)
    assert jl.cls == "jump" and jl.target == 0x80012340 and jl.link == isa.RA
    assert isa.decode(JR_RA).is_unconditional()
    assert isa.decode(i_type(4, 0, 0, 5)).is_unconditional()  # b
    assert isa.decode(r_type(0x0C)).cls == "interp"            # syscall
    assert isa.decode(0x42000010).name == "rfe"
    assert isa.decode(0x4A180001).name == "cop2"               # GTE RTPS
    assert isa.decode(r_type(0x20, 1, 2, 3)).cls == "trap"     # add
    assert isa.decode(0xFC000000).name == "reserved"


def test_load_delay_conflicts():
    lw_v0 = isa.decode(LW(2, 0, 4))
    assert lw_v0.ldest() == 2
    # the next instruction reads v0: the load must stay in flight
    assert isa.conflicts(isa.decode(r_type(0x21, 2, 3, 4)), 2)
    # writes v0 without reading it: committing early is the same as the cancellation
    assert not isa.conflicts(isa.decode(ADDIU(2, 3, 1)), 2)
    # another load into v0: the first must not land after it
    assert isa.conflicts(isa.decode(LW(2, 4, 5)), 2)
    # LWL / LWR forward the value in flight: no conflict (unless v0 is the base)
    assert not isa.conflicts(isa.decode(i_type(0x26, 5, 2, 0)), 2)
    assert isa.conflicts(isa.decode(i_type(0x26, 2, 3, 0)), 2)
    # MTC2 reads rt
    assert isa.conflicts(isa.decode((0x12 << 26) | (4 << 21) | (2 << 16)), 2)


def test_hash_is_fnv1a64_over_words():
    assert recomp_hash([]) == 0xCBF29CE484222325
    h = 0xCBF29CE484222325
    for w in (1, 0xFFFFFFFF):
        h = ((h ^ w) * 0x100000001B3) & (2 ** 64 - 1)
    assert recomp_hash([1, 0xFFFFFFFF]) == h


# ---- input ----
def make_iso(files, raw=False, mode=2):
    """A minimal ISO 9660 image: PVD at 16, root directory at 18, files from 20."""
    sectors = {}
    lba = 20
    entries = []
    for name, data in files:
        entries.append((name, lba, len(data)))
        for k in range(0, max(1, len(data)), 2048):
            sectors[lba] = data[k:k + 2048]
            lba += 1

    def record(name, ext, size, is_dir):
        n = name.encode()
        rec = bytearray(33 + len(n) + (1 - len(n) % 2))
        rec[0] = len(rec)
        struct.pack_into("<I", rec, 2, ext)
        struct.pack_into(">I", rec, 6, ext)
        struct.pack_into("<I", rec, 10, size)
        struct.pack_into(">I", rec, 14, size)
        rec[25] = 2 if is_dir else 0
        rec[32] = len(n)
        rec[33:33 + len(n)] = n
        return bytes(rec)

    root = record("\x00", 18, 2048, True) + record("\x01", 18, 2048, True)
    for name, ext, size in entries:
        root += record(name + ";1", ext, size, False)
    sectors[18] = root
    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[8:40] = b"PLAYSTATION".ljust(32)
    pvd[156:156 + 34] = record("\x00", 18, 2048, True)[:34]
    sectors[16] = bytes(pvd)
    out = bytearray()
    for s in range(lba):
        user = sectors.get(s, b"").ljust(2048, b"\x00")
        if not raw:
            out += user
        else:
            sec = bytearray(2352)
            sec[0:12] = b"\x00" + b"\xff" * 10 + b"\x00"
            sec[15] = mode
            off = 24 if mode == 2 else 16
            sec[off:off + 2048] = user
            out += sec
    return bytes(out)


@pytest.mark.parametrize("raw,mode", [(False, 2), (True, 2), (True, 1)])
def test_disc_boot_exe(tmp_path, raw, mode):
    exe = make_exe(words([ADDIU(2, 0, 7), JR_RA, NOP]))
    cnf = b"BOOT = cdrom:\\SLUS_123.45;1\r\nTCB = 4\r\n"
    img = make_iso([("SYSTEM.CNF", cnf), ("SLUS_123.45", exe)], raw=raw, mode=mode)
    path = tmp_path / ("game.bin" if raw else "game.iso")
    path.write_bytes(img)
    if raw:
        (tmp_path / "game.cue").write_text('FILE "game.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
        path = tmp_path / "game.cue"
    e = load_input(str(path))
    assert e.text_addr == 0x80010000 and e.pc0 == 0x80010000 and e.name == "SLUS_123.45"
    d = Disc(str(path))
    assert d.boot_path() == "\\SLUS_123.45;1"
    d.close()


def test_exe_rejects_garbage():
    with pytest.raises(ValueError):
        Exe(b"\x00" * 4096)


# ---- discovery ----
BASE = 0x80010000


def program():
    """main: calls f (JAL) and a BIOS stub, takes f's address (LUI / ADDIU), switches through a jump table."""
    w = [0] * 64
    # main @ 0x80010000
    w[0] = ADDIU(isa.SP, isa.SP, -24)
    w[1] = SW(isa.RA, 16, isa.SP)
    w[2] = jal(BASE + 0x80)          # f
    w[3] = NOP
    w[4] = jal(BASE + 0xA0)          # bios stub
    w[5] = NOP
    w[6] = LUI(4, BASE >> 16)        # a0 = &g (function pointer)
    w[7] = ADDIU(4, 4, 0xC0)
    # switch (v0) through the table at BASE + 0x100 (3 entries)
    w[8] = i_type(0x0B, 2, 1, 3)     # sltiu at, v0, 3
    w[9] = i_type(4, 1, 0, 8)        # beq at, zero, default (+8 -> w[18])
    w[10] = r_type(0x00, 0, 2, 2, 2)  # sll v0, v0, 2
    w[11] = LUI(1, BASE >> 16)
    w[12] = r_type(0x21, 1, 2, 1)    # addu at, at, v0
    w[13] = LW(2, 0x100, 1)          # lw v0, 0x100(at)
    w[14] = NOP
    w[15] = r_type(0x08, 2)          # jr v0
    w[16] = NOP
    w[17] = NOP
    w[18] = LW(isa.RA, 16, isa.SP)   # default / epilogue
    w[19] = NOP
    w[20] = JR_RA
    w[21] = ADDIU(isa.SP, isa.SP, 24)
    # case blocks after the epilogue
    w[22] = ADDIU(3, 0, 1)
    w[23] = i_type(4, 0, 0, 0xFFFB)  # b w[18]... (-5 + 1)
    w[24] = NOP
    w[25] = ADDIU(3, 0, 2)
    w[26] = i_type(4, 0, 0, 0xFFF8)
    w[27] = NOP
    # f @ +0x80: leaf
    w[32] = ADDIU(2, 4, 1)
    w[33] = JR_RA
    w[34] = NOP
    # bios stub @ +0xA0: li t2, 0xA0 ; jr t2 ; li t1, 0x3C
    w[40] = ADDIU(isa.T2, 0, 0xA0)
    w[41] = r_type(0x08, isa.T2)
    w[42] = ADDIU(isa.T1, 0, 0x3C)
    # g @ +0xC0 (only reached through the pointer)
    w[48] = ADDIU(isa.SP, isa.SP, -8)
    w[49] = JR_RA
    w[50] = ADDIU(isa.SP, isa.SP, 8)
    data = bytearray(words(w))
    # jump table @ +0x100
    data += words([BASE + 22 * 4, BASE + 25 * 4, BASE + 18 * 4])
    return bytes(data)


def test_discovery_finds_calls_pointers_tables_stubs():
    img = Image(BASE, program(), "exe")
    d = Discovery(img, {BASE})
    funcs = {f.start: f for f in d.run()}
    assert set(funcs) >= {BASE, BASE + 0x80, BASE + 0xA0, BASE + 0xC0}
    main_f = funcs[BASE]
    assert main_f.end == BASE + 28 * 4  # the case blocks after the epilogue belong to main (jump table)
    assert d.stats["jump_tables"] == 1
    assert funcs[BASE + 0xA0].name == "bios_A0_3C" and bios_stub(img, BASE + 0xA0) == "A0_3C"
    assert funcs[BASE + 0x80].end == BASE + 0x8C


def test_merge_records():
    a = (0x80100000, b"\x01\x00\x00\x00" * 8)
    b = (0x80100010, b"\x01\x00\x00\x00" * 8)        # overlaps a with the same bytes: merged
    c = (0x80100000, b"\x02\x00\x00\x00" * 8)        # same address, other bytes: another blob (overlay B)
    blobs = merge_records([a, b, c])
    assert len(blobs) == 2
    merged = [bl for bl in blobs if len(bl[2]) == 2][0]
    assert merged[0] == 0x80100000 and len(merged[1]) == 48


def overlay(tag):
    # ovl_entry(x): return x + tag ; plus a helper reached through a pointer table
    return words([ADDIU(2, 4, tag), JR_RA, NOP, NOP])


def test_overlays_at_one_address_become_two_entries(tmp_path):
    exe = Exe(make_exe(words([JR_RA, NOP])))
    disc = tmp_path / "discovered.txt"
    with open(disc, "w") as f:
        for tag in (0x11, 0x22):
            data = overlay(tag)
            h = recomp_hash(list(struct.unpack("<%dI" % (len(data) // 4), data)))
            f.write("80100000 %016X %s\n" % (h, data.hex().upper()))
        f.write("80010000\n")  # an address of the EXE alone
        f.write("# comment\n")
    recs = read_discovered(str(disc))
    assert len(recs) == 3 and recs[2] == (0x80010000, None)
    funcs, stats = build_functions(exe, recs)
    ram = [f for f in funcs if f.image.origin == "ram"]
    assert len(ram) == 2 and {f.start for f in ram} == {0x80100000}
    assert len({f.hash for f in ram}) == 2
    out = tmp_path / "out"
    summary = recompile(exe, str(out), recs, comments=False)
    assert summary["ram_functions"] == 2
    table = (out / "ps1_rc_table.c").read_text()
    assert table.count("0x80100000u") == 2
    assert "ps1_recomp_title_table" in table


def test_generated_code_is_not_lifted(tmp_path):
    exe = Exe(make_exe(words([JR_RA, NOP])))
    recs = [(0x80100000, overlay(k)) for k in range(8)]  # 8 contents at one address: a stub generated at run time
    funcs, stats = build_functions(exe, recs)
    assert stats["volatile_addresses"] == 1
    assert not [f for f in funcs if f.image.origin == "ram"]


def test_same_code_recorded_again_is_one_content():
    # an older runtime recorded one PC again under several lengths (views into other records): one content
    exe = Exe(make_exe(words([JR_RA, NOP])))
    code = overlay(0)
    recs = [(0x80100000, code[:n]) for n in (len(code), 16, 8, 12)] * 3
    assert merge_variants(recs) == {0x80100000: [code]}
    funcs, stats = build_functions(exe, recs)
    assert stats["volatile_addresses"] == 0 and stats["records"] == 1


def test_cli_and_c_compiles(tmp_path, capsys):
    p = tmp_path / "prog.exe"
    p.write_bytes(make_exe(program()))
    out = tmp_path / "src"
    assert main([str(p), str(out)]) == 0
    summary = json.loads(capsys.readouterr().out.strip().splitlines()[-1])
    assert summary["functions"] >= 4 and summary["bios_stubs"] == 1
    files = sorted(os.listdir(out))
    assert "ps1_rc_table.c" in files and "ps1_rc_%08X.c" % BASE in files
    cc = shutil.which(os.environ.get("CC", "cc"))
    if not cc:
        pytest.skip("no C compiler")
    for f in files:
        r = subprocess.run([cc, "-fsyntax-only", "-Wall", "-Wno-unused-label", "-Wno-unused-variable",
                            "-Wno-unused-but-set-variable", "-I", RUNTIME, str(out / f)],
                           capture_output=True, text=True, check=False)
        assert r.returncode == 0, r.stderr[-2000:]


def test_long_bodies_are_cut_off_delay_slots():
    from ps1_tools import discover
    n = discover.MAX_BODY
    w = [ADDIU(2, 2, 1)] * (2 * n + 500) + [JR_RA, NOP]
    w[n - 1] = i_type(4, 2, 3, 1)  # a branch right before the cut: its slot stays with it
    img = Image(BASE, words(w), "exe")
    funcs = sorted(Discovery(img, {BASE}).run(), key=lambda f: f.start)
    assert len(funcs) == 3
    assert funcs[0].end == BASE + (n - 1) * 4  # cut before the branch, not between it and its slot
    assert funcs[-1].end == BASE + len(w) * 4
    assert all(a.end == b.start for a, b in zip(funcs, funcs[1:]))


# ---- blocks, event checks, registers ----
def test_blocks_segments_and_registers():
    from ps1_tools.discover import Function
    from ps1_tools.lift import FnLifter
    w = ([LUI(8, 0x1F80)] + [ADDIU(4, 4, 1)] * 4 + [LW(2, 0x1814, 8)] + [ADDIU(3, 3, 1)] * 6 + [LW(9, 0, isa.SP)]
         + [ADDIU(3, 3, 1)] * 13 + [JR_RA, NOP])
    img = Image(BASE, words(w), "exe")
    c = FnLifter(Function(img, BASE, BASE + 4 * len(w)), "ps1_rc_t").function(comments=False)
    # blocks of 12 instructions: three resume points; the rest answer from the mid_ table
    assert [x for x in c.split("\n") if x.strip().startswith("case ")] == [
        "    case 0x80010000u: goto L_80010000;", "    case 0x80010030u: goto L_80010030;",
        "    case 0x80010060u: goto L_80010060;"]
    assert "mid_[27] = {0, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 11," in c
    # the GPU status register (a LUI base) goes through the core; the stack is a RAM access, handed over otherwise
    assert "RC_LW(r8 + 0x00001814u, 3u, ldv, RC_INTERP(0x80010014u););" in c
    assert "RC_LW_RAM(r29, 3u, ldv, RC_HAND(0x80010030u, 12););" in c
    # one check per segment: up to the IO access (5 cycles before it), after it, then each block (the slot included;
    # the RAM load counts its 3 cycles)
    assert "RC_SEG(0x80010000u, 5, 12);" in c and "RC_SEG(0x80010018u, 5, 6);" in c
    assert "RC_SEG(0x80010030u, 14, 12);" in c and "RC_SEG(0x80010060u, 3, 4);" in c
    assert c.count("RC_SEG(") == 4 and c.count("RC_I();") == len(w)
    # registers: written ones in locals written back, read-only ones const
    assert "    uint32_t r2 = h->r[2], r3 = h->r[3], r4 = h->r[4], r8 = h->r[8], r9 = h->r[9];\n" in c
    assert "    const uint32_t r29 = h->r[29], r31 = h->r[31];\n" in c
    assert "h->r[2] = r2; h->r[3] = r3; h->r[4] = r4; h->r[8] = r8; h->r[9] = r9;" in c and "h->r[29] =" not in c


def test_return_points_and_targets_are_resume_points():
    from ps1_tools.lift import FnLifter
    img = Image(BASE, program(), "exe")
    main_f = [f for f in Discovery(img, {BASE}).run() if f.start == BASE][0]
    c = FnLifter(main_f, "ps1_rc_main").function(comments=False)
    for a in (0, 16, 24, 18 * 4, 22 * 4, 25 * 4):  # entry, after each JAL's slot, jump table targets
        assert "case 0x%08Xu:" % (BASE + a) in c


def test_files_stay_the_same_when_functions_are_found(tmp_path):
    n, size = 300, 20
    w = []
    for k in range(n):
        w += [ADDIU(2, 2, k + 1)] * (size - 2) + [JR_RA, NOP]
    exe = Exe(make_exe(words(w)))
    seeds = [(BASE + 4 * size * k, None) for k in range(n)]
    out1, out2 = tmp_path / "a", tmp_path / "b"
    recompile(exe, str(out1), [s for k, s in enumerate(seeds) if k != n // 2], comments=False)
    recompile(exe, str(out2), seeds, comments=False)
    f1 = {f: (out1 / f).read_text() for f in os.listdir(out1) if f != "ps1_rc_table.c"}
    f2 = {f: (out2 / f).read_text() for f in os.listdir(out2) if f != "ps1_rc_table.c"}
    assert len(f2) >= 4
    assert len([f for f in f2 if f1.get(f) != f2[f]]) == 1  # the new function's file, nothing else
