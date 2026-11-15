"""recomp_source.py (.github/workflows/recompile.yml) on synthetic dumps: links and names, archives, which file of a
download is the dump, and the lift of an Xbox image and of a GBA ROM into the folder the workflow uploads. Runs with
the rest of `pytest tools/` in the linux job. The downloads need curl and a local server; they skip without curl."""
import functools
import http.server
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import threading
import zipfile

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import recomp_source as rs  # noqa: E402

TEST_XBE = os.path.join(ROOT, "tools", "conformance", "test.xbe")
needs_curl = pytest.mark.skipif(shutil.which("curl") is None, reason="curl is not installed")
needs_capstone = pytest.mark.skipif(importlib.util.find_spec("capstone") is None, reason="capstone is not installed")


def gba_rom():
    """A cartridge header (GBATEK) and two ARM instructions: mov r0, #1 ; b ."""
    b = bytearray(0x400)
    struct.pack_into("<I", b, 0, 0xEA00002E)            # b 0x080000C0
    b[0xA0:0xAC] = b"TESTGAME\0\0\0\0"
    b[0xAC:0xB0] = b"ATST"
    b[0xB0:0xB2] = b"01"
    b[0xB2] = 0x96
    b[0xBD] = (-(sum(b[0xA0:0xBD]) + 0x19)) & 0xFF
    struct.pack_into("<II", b, 0xC0, 0xE3A00001, 0xEAFFFFFE)
    return bytes(b)


def xbox_iso(tmp_path, name="game.iso"):
    disc = tmp_path / "disc"
    disc.mkdir(exist_ok=True)
    shutil.copy(TEST_XBE, disc / "default.xbe")
    out = tmp_path / name
    subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "xiso", "mkxiso.py"), str(disc), str(out)])
    shutil.rmtree(disc)
    return out


# ---- links and names ----

def test_direct_url(monkeypatch):
    monkeypatch.delenv("HF_TOKEN", raising=False)
    hf = "https://huggingface.co/datasets/me/dumps/blob/main/My%20Game.iso"
    assert rs.direct_url(hf) == ("https://huggingface.co/datasets/me/dumps/resolve/main/My%20Game.iso", [], None)
    assert rs.direct_url("https://huggingface.co/me/repo/blob/main/a/blob/g.iso")[0] == \
        "https://huggingface.co/me/repo/resolve/main/a/blob/g.iso"
    monkeypatch.setenv("HF_TOKEN", "hf_abc")
    assert rs.direct_url(hf)[1] == ["--header", "Authorization: Bearer hf_abc"]
    # the token goes to Hugging Face only
    assert rs.direct_url("https://example.org/blob/g.iso") == ("https://example.org/blob/g.iso", [], None)
    assert rs.direct_url("https://www.dropbox.com/s/abc/g.iso?dl=0")[0] == "https://www.dropbox.com/s/abc/g.iso?dl=1"
    assert rs.direct_url("https://drive.google.com/file/d/ID/view?usp=sharing")[2] == "gdrive"


def test_names():
    cd = "HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename*=UTF-8''My%20Rom.gba; filename=\"x.gba\"\r\n"
    assert rs.header_name(cd) == "My Rom.gba"
    assert rs.header_name('content-disposition: inline; filename="a b.iso"\r\n') == "a b.iso"
    assert rs.header_name("Content-Type: text/plain\r\n") is None
    # the link's own name when it ends like a dump, the server's otherwise, the given one before both
    assert rs.pick_name("https://h/x/My%20Game%20%28USA%29.iso?download=true", cd, "") == "My Game (USA).iso"
    assert rs.pick_name("https://h/dl?id=5", cd, "https://cdn/blob/abcdef") == "My Rom.gba"
    assert rs.pick_name("https://h/dl?id=5", "", "https://cdn/files/pack.zip") == "pack.zip"
    assert rs.pick_name("https://h/dl?id=5", cd, "", given="../../etc/disc.iso") == "disc.iso"
    assert rs.pick_name("https://h/", "", "") == "download"
    assert rs.safe_name("..\\..\\.hidden") == "hidden"


# ---- archives ----

def test_archive_kind_and_volumes(tmp_path):
    z = tmp_path / "noext"
    with zipfile.ZipFile(z, "w") as zf:
        zf.writestr("a.txt", "a")
    assert rs.archive_kind(str(z)) == "zip"                 # by its first bytes
    rom = tmp_path / "game.gba"
    rom.write_bytes(b"PK\x03\x04" + bytes(64))
    assert rs.archive_kind(str(rom)) is None                # a dump's name is never an archive
    for n in ("g.part1.rar", "g.part2.rar", "g.7z.001", "g.7z.002", "other.part1.rar"):
        (tmp_path / n).write_bytes(b"x")
    first, mates = rs.volumes_of(str(tmp_path / "g.part2.rar"))
    assert not first and [os.path.basename(m) for m in mates] == ["g.part1.rar", "g.part2.rar"]
    first, mates = rs.volumes_of(str(tmp_path / "g.7z.001"))
    assert first and [os.path.basename(m) for m in mates] == ["g.7z.001", "g.7z.002"]
    assert rs.volumes_of(str(z)) == (True, [str(z)])


def test_unpack_all(tmp_path):
    inner = tmp_path / "inner.zip"
    with zipfile.ZipFile(inner, "w") as zf:
        zf.writestr("sub dir/game.gba", gba_rom())
    with zipfile.ZipFile(tmp_path / "outer.zip", "w") as zf:
        zf.write(inner, "inner.zip")
        zf.writestr("readme.txt", "hello")
    inner.unlink()
    assert rs.unpack_all(str(tmp_path)) == ["outer.zip", "inner.zip"]
    assert sorted(os.path.relpath(p, tmp_path) for p in rs.list_files(str(tmp_path))) == \
        ["readme.txt", os.path.join("sub dir", "game.gba")]


# ---- which file is the dump ----

def test_find_source(tmp_path):
    (tmp_path / "notes.txt").write_text("x")
    with pytest.raises(rs.Failed, match="no dump"):
        rs.find_source(str(tmp_path))
    # an extracted Xbox disc: default.xbe, whatever a data file's first bytes look like
    game = tmp_path / "xbox"
    (game / "data").mkdir(parents=True)
    shutil.copy(TEST_XBE, game / "default.xbe")
    (game / "data" / "level.dat").write_bytes(gba_rom())
    src, con, others = rs.find_source(str(game))
    assert (os.path.basename(src), con) == ("default.xbe", "xbox")
    assert others == [os.path.join("data", "level.dat")]
    # a ROM under its own extension goes first
    (game / "rom.gba").write_bytes(gba_rom())
    assert rs.find_source(str(game))[:2] == (str(game / "rom.gba"), "gba")
    with pytest.raises(rs.Failed, match="not a dump"):
        rs.find_source(str(tmp_path / "notes.txt"))


def test_find_source_xbox_image(tmp_path):
    iso = xbox_iso(tmp_path)
    (tmp_path / "other.iso").write_bytes(bytes(0x20000))        # no XDVDFS, no ISO 9660: not a dump
    assert rs.find_source(str(tmp_path)) == (str(iso), "xbox", [])


# ---- the lift ----

@needs_capstone
def test_lift_xbox(tmp_path, monkeypatch):
    (tmp_path / "dl").mkdir()
    iso = xbox_iso(tmp_path / "dl")
    out, gh_out, gh_sum = tmp_path / "src", tmp_path / "gh_out", tmp_path / "gh_sum"
    monkeypatch.setenv("GITHUB_OUTPUT", str(gh_out))
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(gh_sum))
    assert rs.main(["lift", str(tmp_path / "dl"), "--out", str(out), "--work", str(tmp_path / "work")]) == 0
    assert (out / "CMakeLists.txt").is_file() and (out / "src" / "main.c").is_file()
    assert (out / "src" / "recomp" / "gen" / "recomp_dispatch.c").is_file()
    info = json.loads((out / "recomp_info.json").read_text())
    assert info["console"] == "xbox" and info["key"] == "12345678" and info["c_files"] >= 4
    assert "-DXBOXRECOMP_DIR" in (out / "README.md").read_text()
    assert gh_out.read_text() == "name=recomp-src-xbox-12345678\npath=%s\n" % out
    assert "recomp-src-xbox-12345678" in gh_sum.read_text()
    # nothing of the dump in what is uploaded
    names = [os.path.basename(p).lower() for p in rs.list_files(str(out))]
    assert not [n for n in names if n.endswith((".xbe", ".iso"))]
    assert iso.is_file()


@needs_capstone
def test_lift_xbox_other_images(tmp_path, monkeypatch):
    """XBA-82: an image default.xbe names is lifted too, as its own project under images/."""
    monkeypatch.delenv("GITHUB_OUTPUT", raising=False)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    disc = tmp_path / "disc"
    (disc / "sub").mkdir(parents=True)
    shutil.copy(TEST_XBE, disc / "sub" / "game.xbe")
    with open(TEST_XBE, "rb") as f:
        (disc / "default.xbe").write_bytes(f.read() + b"d:\\sub\\game.xbe\0")
    out = tmp_path / "src"
    assert rs.main(["lift", str(disc), "--out", str(out), "--work", str(tmp_path / "work")]) == 0
    info = json.loads((out / "recomp_info.json").read_text())
    assert info["images"] == [{"path": "sub/game.xbe", "lifted": True, "project": "images/sub_game.xbe"}]
    assert (out / "images" / "sub_game.xbe" / "src" / "recomp" / "gen" / "recomp_dispatch.c").is_file()
    assert not [p for p in rs.list_files(str(out)) if p.lower().endswith(".xbe")]


def test_lift_gba(tmp_path, monkeypatch):
    monkeypatch.delenv("GITHUB_OUTPUT", raising=False)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    rom = tmp_path / "game.gba"
    rom.write_bytes(gba_rom())
    out = tmp_path / "src"
    assert rs.main(["lift", str(rom), "--out", str(out)]) == 0
    assert (out / "gba_rc_table.c").is_file()
    info = json.loads((out / "recomp_info.json").read_text())
    assert info["console"] == "gba" and info["key"].startswith("ATST-") and info["title"] == "TESTGAME"
    assert info["functions"] == 1
    report = json.loads((out / "report.json").read_text())
    assert report["files"] == 2 and report["header"]["game_code"] == "ATST"
    assert "gba_play" in (out / "README.md").read_text()
    assert not (out / "game.gba").exists()
    # the dump is never inside what gets uploaded
    assert rs.main(["lift", str(rom), "--out", str(tmp_path)]) == 1


def test_lift_refuses_what_is_no_dump(tmp_path, capsys):
    (tmp_path / "dl").mkdir()
    (tmp_path / "dl" / "movie.iso").write_bytes(bytes(0x20000))
    assert rs.main(["lift", str(tmp_path / "dl"), "--out", str(tmp_path / "src")]) == 1
    assert "no dump this toolkit lifts" in capsys.readouterr().err
    assert not (tmp_path / "src").exists()


# ---- downloads ----

class Handler(http.server.SimpleHTTPRequestHandler):
    """The served folder, plus: /dl (a name only in Content-Disposition), /redir (to pack.zip), /flaky (drops the
    first transfer half way, then serves ranges), /token (wants the Hugging Face header: it must not get it)."""
    hits = {}

    def log_message(self, *a):
        pass

    def _send(self, data, code=200, extra=()):
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?")[0]
        Handler.hits[path] = Handler.hits.get(path, 0) + 1
        rom = gba_rom()
        if path == "/dl":
            return self._send(rom, extra=[("Content-Disposition", "attachment; filename=\"My Rom.gba\"")])
        if path == "/redir":
            self.send_response(302)
            self.send_header("Location", "/pack.zip")
            self.end_headers()
            return None
        if path == "/token":
            return self._send(b"", 403 if self.headers.get("Authorization") else 404)
        if path == "/flaky/game.gba":
            rng = self.headers.get("Range")
            if rng:
                start = int(rng.split("=")[1].split("-")[0])
                span = "bytes %d-%d/%d" % (start, len(rom) - 1, len(rom))
                return self._send(rom[start:], 206, [("Content-Range", span)])
            self.send_response(200)
            self.send_header("Content-Length", str(len(rom)))
            self.end_headers()
            self.wfile.write(rom[:300])
            self.wfile.flush()
            self.connection.close()
            return None
        return super().do_GET()


@pytest.fixture
def server(tmp_path):
    served = tmp_path / "served"
    served.mkdir()
    (served / "My Game.gba").write_bytes(gba_rom())
    (served / "page.iso").write_text("<!DOCTYPE html><html>sign in</html>")
    with zipfile.ZipFile(served / "pack.zip", "w") as zf:
        zf.writestr("dir/game.gba", gba_rom())
    Handler.hits = {}
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Handler, directory=str(served)))
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    yield "http://127.0.0.1:%d" % httpd.server_address[1]
    httpd.shutdown()
    httpd.server_close()


@needs_curl
def test_fetch(tmp_path, server, monkeypatch, capsys):
    monkeypatch.setattr(rs, "RETRY_WAIT", 0)
    monkeypatch.setenv("HF_TOKEN", "hf_abc")
    dest = tmp_path / "dl"

    def fetch(*urls, name=None, sub="x"):
        d = dest / sub
        rc = rs.main(["fetch", "--dest", str(d)] + (["--name", name] if name else []) + list(urls))
        return rc, sorted(os.path.relpath(p, d) for p in rs.list_files(str(d))) if d.is_dir() else []

    assert fetch(server + "/My%20Game.gba", sub="a") == (0, ["My Game.gba"])
    assert (dest / "a" / "My Game.gba").read_bytes() == gba_rom()
    assert fetch(server + "/dl?id=5", sub="b") == (0, ["My Rom.gba"])
    assert fetch(server + "/dl?id=5", name="x.gba", sub="c") == (0, ["x.gba"])
    assert fetch(server + "/redir", server + "/My%20Game.gba", sub="d") == \
        (0, ["My Game.gba", os.path.join("dir", "game.gba")])
    # a transfer cut half way is resumed, not started over, and the bytes are the file's
    assert fetch(server + "/flaky/game.gba", sub="e") == (0, ["game.gba"])
    assert (dest / "e" / "game.gba").read_bytes() == gba_rom() and Handler.hits["/flaky/game.gba"] == 2
    capsys.readouterr()
    # what is not a file: a page, a missing file (asked once: 404 is final), a second name for two links
    assert fetch(server + "/page.iso", sub="f") == (1, [])
    assert "web page" in capsys.readouterr().err
    assert fetch(server + "/gone.iso", sub="g") == (1, []) and Handler.hits["/gone.iso"] == 1
    assert "404" in capsys.readouterr().err
    assert fetch(server + "/token", sub="h")[0] == 1        # 404: no Authorization header left this host's way
    assert "404" in capsys.readouterr().err
    assert fetch(server + "/dl", server + "/dl", name="x.gba", sub="i")[0] == 1
    assert fetch("ftp://host/game.iso", sub="j")[0] == 1
