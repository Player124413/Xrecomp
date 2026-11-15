#!/usr/bin/env python3
"""A game dump to its recompiled C sources, nothing built: what .github/workflows/recompile.yml runs.

    python3 tools/ci/recomp_source.py fetch --dest DIR [--name FILE] URL [URL...]
    python3 tools/ci/recomp_source.py lift SOURCE --out DIR [--work DIR]

fetch  downloads each URL into DIR (curl, resumed and retried; a Hugging Face « blob » page link becomes its file
       link and takes $HF_TOKEN for a private repository; a Dropbox link its direct form; a Google Drive link goes
       through gdown) and unpacks what is an archive (zip / 7z / rar / gz / xz / bz2, split volumes included), so
       DIR ends up holding the dump itself. --name gives the file its name when the link has none worth keeping.

lift   SOURCE is the dump or a folder holding it (fetch's DIR). The console is read from the file, as tools.prepare
       does, and the lift alone runs:

         Xbox            python -m tools.prepare SOURCE --no-build   -> the title project (CMake + C), and the same
                         for each other image default.xbe launches (XBA-82), under images/
         GBA, PS1, N64   python -m <console>_tools.recomp SOURCE DIR -> the lifted C files and the lifter's report

       DIR gets those sources, README.md (how to build them) and recomp_info.json. The dump, its executable and its
       data never go into DIR. With $GITHUB_OUTPUT / $GITHUB_STEP_SUMMARY set, the artifact's name and a summary
       are written there.

The sources are lifted from the game's own code: they are for whoever owns the dump, as the toolkit's rule goes.
"""

import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

ARCHIVE_EXTS = ("zip", "7z", "rar", "gz", "tgz", "xz", "bz2", "tar")
GAME_EXTS = ("iso", "xiso", "cso", "ciso", "zso", "cci", "xbe", "gba", "z64", "v64", "n64", "cue", "bin", "img",
             "chd", "exe")
ARCHIVE_MAGIC = ((b"PK\x03\x04", "zip"), (b"7z\xbc\xaf\x27\x1c", "7z"), (b"Rar!\x1a\x07", "rar"), (b"\x1f\x8b", "gz"),
                 (b"\xfd7zXZ\x00", "xz"), (b"BZh", "bz2"))
# A split archive's volumes: only the first is opened, the others go with it.
VOLUME = re.compile(r"^.+?(?:\.part(?P<rar>\d+)\.rar|\.(?:7z|zip)\.(?P<num>\d{3}))$", re.I)
UNPACK_ROUNDS = 3               # an archive in an archive (a .tar.gz, a zip of a 7z)
OTHER_IMAGES_MAX = 8            # as the Android prepare's IMAGES_MAX
ATTEMPTS = 8                    # of one download, each resuming the one before
RETRY_WAIT = 5                  # seconds between them
FINAL_CURL_ERRORS = (1, 3, 22)  # unsupported protocol, malformed URL, an HTTP error (--fail)
CONSOLE_NAMES = {"xbox": "Xbox", "gba": "Game Boy Advance", "ps1": "PlayStation", "n64": "Nintendo 64"}


class Failed(Exception):
    """What went wrong, in words for the workflow's log."""


def say(msg):
    print(msg, flush=True)


def ext_of(name):
    return os.path.splitext(name)[1][1:].lower()


def human(n):
    for unit in ("bytes", "KB", "MB"):
        if n < 1024:
            return "%d %s" % (n, unit)
        n /= 1024.0
    return "%.2f GB" % n


# ── fetch ───────────────────────────────────────────────────────

def direct_url(url):
    """(the URL to download, extra curl arguments, "gdrive" or None): the hosts whose share links are not the file."""
    u = urllib.parse.urlsplit(url)
    host = (u.hostname or "").lower()
    args = []
    if host in ("huggingface.co", "hf.co") or host.endswith(".huggingface.co"):
        path = re.sub(r"^((?:/(?:datasets|spaces))?/[^/]+/[^/]+)/blob/", r"\1/resolve/", u.path)
        url = urllib.parse.urlunsplit((u.scheme, u.netloc, path, u.query, ""))
        token = os.environ.get("HF_TOKEN", "").strip()
        if token:                           # curl drops it when the redirect leaves the host
            args = ["--header", "Authorization: Bearer " + token]
    elif host.endswith("dropbox.com"):
        q = [(k, v) for k, v in urllib.parse.parse_qsl(u.query, keep_blank_values=True) if k != "dl"]
        url = urllib.parse.urlunsplit((u.scheme, u.netloc, u.path, urllib.parse.urlencode(q + [("dl", "1")]), ""))
    elif host in ("drive.google.com", "docs.google.com", "drive.usercontent.google.com"):
        return url, args, "gdrive"
    return url, args, None


def safe_name(name):
    """A file name out of whatever a URL or a header said: no directory, no control character, never hidden."""
    name = name.replace("\\", "/").split("/")[-1]
    name = "".join(ch for ch in name if ch >= " " and ch not in '<>:"|?*').strip().lstrip(".")
    return name[:200]


def header_name(headers):
    """The file name of the last Content-Disposition in curl's header dump, or None."""
    found = None
    for line in headers.splitlines():
        if not line.lower().startswith("content-disposition:"):
            continue
        m = re.search(r"filename\*\s*=\s*[^']*'[^']*'([^;\r\n]+)", line, re.I)
        if m:
            found = urllib.parse.unquote(m.group(1).strip())
            continue
        m = re.search(r'filename\s*=\s*"([^"]*)"', line, re.I) or re.search(r"filename\s*=\s*([^;\r\n]+)", line, re.I)
        if m:
            found = m.group(1).strip()
    return found


def pick_name(url, headers, effective, given=None):
    """The downloaded file's name: the one given, else the link's when it ends like a dump or an archive, else the
    server's (Content-Disposition), else the last link's after the redirects."""
    if given:
        return safe_name(given) or "download"
    known = GAME_EXTS + ARCHIVE_EXTS

    def base(u):
        return safe_name(urllib.parse.unquote(urllib.parse.urlsplit(u).path.rstrip("/")))
    first, last, served = base(url), base(effective or ""), safe_name(header_name(headers) or "")
    for cand in (first, served, last):
        if cand and ext_of(cand) in known:
            return cand
    return served or first or last or "download"


def _size(path):
    """A file's size, or the sum of a folder's files (a download in progress: whatever is there now)."""
    try:
        if not os.path.isdir(path):
            return os.path.getsize(path)
        return sum(os.path.getsize(os.path.join(path, n)) for n in os.listdir(path))
    except OSError:
        return 0


def _watch(path, stop):
    """One line every 20 s while a download runs: a log that stays silent for minutes looks stuck."""
    last = t0 = time.time()
    while not stop.wait(1):
        if time.time() - last >= 20:
            last = time.time()
            say("      %s in %d s" % (human(_size(path)), last - t0))


def watched(path, cmd, **kw):
    """subprocess.run(cmd) with _watch on path."""
    stop = threading.Event()
    t = threading.Thread(target=_watch, args=(path, stop), daemon=True)
    t.start()
    try:
        return subprocess.run(cmd, universal_newlines=True, errors="replace", **kw)
    finally:
        stop.set()
        t.join()


def curl(url, dest, extra, resume=True):
    """curl URL into dest; (exit code, headers, the URL after the redirects, stderr)."""
    cmd = ["curl", "--fail", "--location", "--silent", "--show-error", "--connect-timeout", "30",
           "--retry", "3", "--retry-delay", "5", "--speed-limit", "1024", "--speed-time", "120",
           "--dump-header", dest + ".hdr", "--output", dest, "--write-out", "%{url_effective}"]
    if resume:
        cmd += ["--continue-at", "-"]
    p = watched(dest, cmd + extra + ["--", url], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    headers = ""
    try:
        with open(dest + ".hdr", encoding="latin-1") as f:
            headers = f.read()
        os.remove(dest + ".hdr")
    except OSError:
        pass
    return p.returncode, headers, p.stdout.strip(), p.stderr.strip()


def looks_like_a_page(path):
    with open(path, "rb") as f:
        head = f.read(512).lstrip().lower()
    return head.startswith((b"<!doctype html", b"<html", b"<head", b"<?xml"))


def place(tmp, dest_dir, name):
    """tmp renamed to dest_dir/name, numbered when that name is taken."""
    stem, ext = os.path.splitext(name)
    out, n = os.path.join(dest_dir, name), 1
    while os.path.exists(out):
        n += 1
        out = os.path.join(dest_dir, "%s-%d%s" % (stem, n, ext))
    os.replace(tmp, out)
    return out


def fetch_gdrive(url, dest_dir, given):
    """A Google Drive share link, through gdown (installed when missing). Its output names the link: kept out of
    the log, the end of it shown without links when it fails."""
    if importlib.util.find_spec("gdown") is None:
        subprocess.call([sys.executable, "-m", "pip", "install", "--quiet", "gdown"])
    tmp = os.path.join(dest_dir, ".gdrive")
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    p = watched(tmp, [sys.executable, "-m", "gdown", url, "-O", tmp + os.sep, "--retries", "3", "--quiet"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    got = [os.path.join(tmp, n) for n in sorted(os.listdir(tmp))]
    got = [g for g in got if os.path.isfile(g)]
    if p.returncode != 0 or not got:
        shutil.rmtree(tmp, ignore_errors=True)
        why = re.sub(r"https?://\S+|[\w-]{20,}", "<link>", "\n".join(p.stdout.strip().splitlines()[-6:]))
        raise Failed("Google Drive did not hand the file over (gdown exit %d): share it as « anyone with the link », "
                     "or host it somewhere that gives a direct link\n%s" % (p.returncode, why))
    out = [place(g, dest_dir, safe_name(given) if given and len(got) == 1 else os.path.basename(g)) for g in got]
    shutil.rmtree(tmp, ignore_errors=True)
    return out


def fetch_one(url, dest_dir, index, given=None):
    """[the files] one URL gave, in dest_dir."""
    if not re.match(r"https?://", url, re.I):
        raise Failed("link %d is not an http(s) link" % index)
    url, extra, special = direct_url(url)
    if special == "gdrive":
        return fetch_gdrive(url, dest_dir, given)
    tmp = os.path.join(dest_dir, ".part-%d" % index)
    # curl retries what the server calls transient (408, 429, 5xx) by itself. A transfer that breaks half way (a
    # multi-GB image over a CDN) is resumed here from what is on disk; an answer such as 404 is final.
    resume = True
    for attempt in range(1, ATTEMPTS + 1):
        rc, headers, effective, err = curl(url, tmp, extra, resume)
        no_resume = rc in (33, 36) or re.search(r"resume|byte ranges|\b416\b", err, re.I)
        if rc == 0 or attempt == ATTEMPTS or (rc in FINAL_CURL_ERRORS and not (no_resume and resume)):
            break
        if no_resume and resume:                # a server that cannot resume: from the start
            resume = False
            if os.path.exists(tmp):
                os.remove(tmp)
        say("      interrupted (%s), going on" % (err.splitlines()[-1] if err else "curl exit %d" % rc))
        time.sleep(RETRY_WAIT)
    if rc != 0 or not os.path.isfile(tmp):
        if os.path.exists(tmp):
            os.remove(tmp)
        why = err.splitlines()[-1] if err else "curl exit %d" % rc
        hint = ""
        if re.search(r"\b40[13]\b", why):
            hint = " (a private Hugging Face repository needs the HF_TOKEN secret)"
        raise Failed("link %d did not download: %s%s" % (index, why, hint))
    if os.path.getsize(tmp) == 0:
        os.remove(tmp)
        raise Failed("link %d gave an empty file" % index)
    if looks_like_a_page(tmp):
        os.remove(tmp)
        raise Failed("link %d gave a web page, not a file: it has to be a direct download link" % index)
    return [place(tmp, dest_dir, pick_name(url, headers, effective, given))]


def archive_kind(path):
    """"zip", "7z", "rar", ... when path is an archive: by its name, or by its first bytes when the name is not a
    dump's (a GBA ROM's first bytes are code: never guessed)."""
    ext = ext_of(path)
    if ext in GAME_EXTS:
        return None
    if VOLUME.match(os.path.basename(path)):
        return "volume"
    try:
        with open(path, "rb") as f:
            head = f.read(8)
    except OSError:
        return None
    for magic, kind in ARCHIVE_MAGIC:
        if head.startswith(magic):
            return kind
    return ext if ext in ARCHIVE_EXTS else None


def volume_key(name):
    """(what the volumes of one split archive share, this one's number), or (None, 0) for any other file."""
    m = VOLUME.match(name)
    if not m:
        return None, 0
    g = "rar" if m.group("rar") else "num"
    return (name[:m.start(g)] + "#" + name[m.end(g):]).lower(), int(m.group(g))


def volumes_of(path):
    """(True when path is the first volume or no volume at all, every file of its set)."""
    d, name = os.path.split(path)
    key, number = volume_key(name)
    if key is None:
        return True, [path]
    return number == 1, sorted(os.path.join(d, n) for n in os.listdir(d) if volume_key(n)[0] == key)


def seven_zip():
    for exe in ("7z", "7zz", "7za"):
        if shutil.which(exe):
            return exe
    return None


def unpack(path, kind):
    """Extracts one archive next to itself; raises Failed."""
    dest = os.path.dirname(path)
    z = seven_zip()
    if z:
        p = subprocess.run([z, "x", "-y", "-bso0", "-bsp0", "-o" + dest, path], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, universal_newlines=True, errors="replace")
        if p.returncode == 0:
            return
        why = (p.stdout.strip().splitlines() or ["exit %d" % p.returncode])[-1]
    else:
        why = "7z is not installed"
    if kind == "zip" or zipfile.is_zipfile(path):
        try:
            with zipfile.ZipFile(path) as zf:
                zf.extractall(dest)
            return
        except (zipfile.BadZipFile, NotImplementedError, OSError, RuntimeError) as e:
            why = str(e)
    elif kind == "rar" and shutil.which("unrar"):
        if subprocess.call(["unrar", "x", "-y", "-o+", "-inul", path, dest + os.sep]) == 0:
            return
        why = "unrar could not read it (a password?)"
    raise Failed("%s did not unpack: %s" % (os.path.basename(path), why))


def unpack_all(dest_dir):
    """Every archive under dest_dir replaced by what it holds; the names unpacked."""
    done = []
    for _ in range(UNPACK_ROUNDS):
        found = []
        for d, _, files in os.walk(dest_dir):
            for fn in sorted(files):
                p = os.path.join(d, fn)
                kind = archive_kind(p)
                if kind:
                    found.append((p, kind))
        if not found:
            break
        for p, kind in found:
            if not os.path.exists(p):           # a volume that went with its first one
                continue
            first, mates = volumes_of(p)
            if not first:
                continue
            say("    unpacking %s (%s)" % (os.path.basename(p), human(sum(os.path.getsize(m) for m in mates))))
            unpack(p, kind)
            for m in mates:
                os.remove(m)
            done.append(os.path.basename(p))
        leftovers = [p for p, _ in found if os.path.exists(p)]
        if leftovers:                           # volumes without their first part
            raise Failed("%s: a volume of a split archive whose first part is missing"
                         % os.path.basename(leftovers[0]))
    return done


def list_files(root):
    out = []
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for fn in sorted(files):
            if not fn.startswith("."):
                out.append(os.path.join(d, fn))
    return out


def cmd_fetch(a):
    if a.name and len(a.url) > 1:
        raise Failed("a file name goes with one link, not %d" % len(a.url))
    os.makedirs(a.dest, exist_ok=True)
    for i, url in enumerate(a.url, 1):
        say("[%d/%d] downloading" % (i, len(a.url)))
        for p in fetch_one(url, a.dest, i, a.name):
            say("    %s (%s)" % (os.path.basename(p), human(os.path.getsize(p))))
    unpack_all(a.dest)
    files = list_files(a.dest)
    say("ready: %d file(s), %s" % (len(files), human(sum(os.path.getsize(p) for p in files))))
    for p in files[:40]:
        say("    %s (%s)" % (os.path.relpath(p, a.dest), human(os.path.getsize(p))))
    return 0


# ── lift ────────────────────────────────────────────────────────

def console_of(path):
    """"n64", "gba", "ps1" or "xbox" for a file tools.prepare would take, in its own order; None for any other."""
    from tools.prepare import gba, n64, ps1
    from tools.xiso.source import is_image_name
    if n64.is_n64(path):
        return "n64"
    if gba.is_gba(path):
        return "gba"
    if ps1.is_ps1(path):
        return "ps1"
    if os.path.basename(path).lower() == "default.xbe":
        return "xbox"
    if is_image_name(path):
        from tools.xiso.xdvdfs import Xiso
        try:
            with Xiso(path) as iso:
                return "xbox" if iso.find("default.xbe") is not None else None
        except Exception:               # XisoError, SourceError, a short file: not an Xbox image
            return None
    return None


IMAGE_EXTS = ("iso", "xiso", "cso", "ciso", "zso", "cci")
ROM_EXTS = {"gba": ("gba",), "n64": ("z64", "v64", "n64")}


def _rank(path, con):
    """Which of a folder's candidates is the dump: a PS1 .cue or .chd before the .bin it names, a disc image or a
    ROM under its own extension next, then default.xbe (an extracted disc), then what only its first bytes give
    away (a data file of an extracted disc can look like a ROM header)."""
    ext = ext_of(path)
    if ext in ("cue", "chd"):
        return 0
    if ext in IMAGE_EXTS or ext in ROM_EXTS.get(con, ()):
        return 1
    if os.path.basename(path).lower() == "default.xbe":
        return 2
    return 3


def find_source(source):
    """(file, console, the other candidates' names) for a dump or the folder that holds it."""
    if os.path.isfile(source):
        con = console_of(source)
        if con is None:
            raise Failed("%s is not a dump this toolkit lifts (an Xbox disc image or default.xbe, a GBA ROM, a PS1 "
                         "disc or PS-X EXE, an N64 ROM)" % os.path.basename(source))
        return source, con, []
    if not os.path.isdir(source):
        raise Failed("%s: no such file or folder" % source)
    from tools.xiso.source import split_parts
    files = list_files(source)
    cue_dirs = {os.path.dirname(p) for p in files if ext_of(p) == "cue"}
    found = []
    for p in files:
        if ext_of(p) in ("bin", "img") and os.path.dirname(p) in cue_dirs:
            continue                            # a track of the .cue next to it
        con = console_of(p)
        if con is None:
            continue
        if con == "xbox" and ext_of(p) != "xbe" and os.path.abspath(split_parts(p)[0]) != os.path.abspath(p):
            continue                            # a later part of a split image
        found.append((_rank(p, con), os.path.relpath(p, source).lower(), p, con))
    if not found:
        names = ", ".join(os.path.relpath(p, source) for p in files[:12]) or "nothing"
        raise Failed("no dump this toolkit lifts in the download (%s). It takes an Xbox disc image (.iso / .xiso / "
                     ".cso / .zso / .cci) or default.xbe, a GBA ROM, a PS1 disc (.cue + .bin / .iso / .chd) or an "
                     "N64 ROM. A link without a file extension needs the file name given." % names)
    found.sort()
    return found[0][2], found[0][3], [os.path.relpath(f[2], source) for f in found[1:]]


def run_logged(cmd, log, env=None):
    """cmd with its output in `log`; raises Failed with the end of it."""
    with open(log, "ab") as f:
        f.write(("\n$ %s\n" % " ".join(cmd)).encode())
        f.flush()
        rc = subprocess.call(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env)
    if rc != 0:
        with open(log, errors="replace") as f:
            tail = f.read()[-4000:]
        raise Failed("%s failed (exit %d)\n--- the end of its log ---\n%s" % (" ".join(cmd[1:3]), rc, tail))


def toolkit_commit():
    from tools.prepare.__main__ import toolkit_version
    return os.environ.get("GITHUB_SHA") or toolkit_version()


def toolkit_url():
    server, repo = os.environ.get("GITHUB_SERVER_URL"), os.environ.get("GITHUB_REPOSITORY")
    return "%s/%s" % (server, repo) if server and repo else "<this toolkit's repository>"


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def tree_stats(root):
    """(C and header files, bytes) under root."""
    n = size = 0
    for p in list_files(root):
        if p.endswith((".c", ".h")):
            n += 1
            size += os.path.getsize(p)
    return n, size


def lift_xbox(src, out, work):
    """tools.prepare --no-build: the title project moves to out, each other image's to out/images/<path>."""
    py = sys.executable
    main_work = os.path.join(work, "main")
    cmd = [py, "-m", "tools.prepare", src, "--work", main_work, "--no-build"]
    say("$ " + " ".join(cmd[1:]))
    if subprocess.call(cmd, cwd=ROOT, stdin=subprocess.DEVNULL) != 0:
        log = os.path.join(main_work, "prepare.log")
        if os.path.isfile(log):
            with open(log, errors="replace") as f:
                say("--- the end of prepare.log ---\n" + f.read()[-4000:])
        raise Failed("the lift failed (tools.prepare)")
    proj = os.path.join(main_work, "project")
    if not os.path.isdir(os.path.join(proj, "src", "recomp", "gen")):
        raise Failed("tools.prepare wrote no project in %s" % proj)
    shutil.rmtree(out, ignore_errors=True)
    shutil.move(proj, out)
    xbe_info = read_json(os.path.join(main_work, "xbe_info.json"))
    stamp = read_json(os.path.join(main_work, "stamp.json"))
    tid = str(xbe_info.get("title_id", ""))[2:].upper() or "UNKNOWN"
    info = {"console": "xbox", "key": tid, "title": (xbe_info.get("title") or "").strip() or tid,
            "entry_point": xbe_info.get("entry_point"), "xdk_version": xbe_info.get("xdk_version"),
            "lift_health": stamp.get("lift_health"), "images": []}
    if os.path.isfile(os.path.join(main_work, "prepare.log")):
        shutil.copy(os.path.join(main_work, "prepare.log"), os.path.join(out, "prepare.log"))

    # XBA-82: a disc whose default.xbe is a launcher keeps its game in other images; without them the sources
    # would be the launcher's alone. One that does not lift is left out, as the Android prepare does.
    xbe = src if src.lower().endswith(".xbe") else os.path.join(main_work, "xbe", "default.xbe")
    disc = os.path.dirname(src) if src.lower().endswith(".xbe") else src
    try:
        from tools.triage import _named_xbes, disc_xbes
        with open(xbe, "rb") as f:
            named = _named_xbes(f.read())
        others = [(rel, p) for rel, p in disc_xbes(disc, main_work, named) if rel.lower() != "default.xbe"]
    except Exception as e:
        say("      the disc's other images were not looked for: %s" % e)
        others = []
    for rel, p in others[:OTHER_IMAGES_MAX]:
        name = re.sub(r"[^A-Za-z0-9._-]", "_", rel)
        sub = os.path.join(work, "images", name)
        say("      another image the title launches: %s" % rel)
        rc = subprocess.call([py, "-m", "tools.prepare", p, "--work", sub, "--no-build"], cwd=ROOT,
                             stdin=subprocess.DEVNULL)
        sub_proj = os.path.join(sub, "project")
        if rc != 0 or not os.path.isdir(sub_proj):
            say("      %s left out: its lift failed" % rel)
            info["images"].append({"path": rel, "lifted": False})
            continue
        os.makedirs(os.path.join(out, "images"), exist_ok=True)
        shutil.move(sub_proj, os.path.join(out, "images", name))
        info["images"].append({"path": rel, "lifted": True, "project": "images/" + name})
    return info


def lift_console(con, src, out, work):
    """<console>_tools.recomp SOURCE out: the lifted C, and the lifter's summary (report.json)."""
    from tools.prepare import gba, n64, ps1
    mod = {"gba": gba, "ps1": ps1, "n64": n64}[con]
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    os.makedirs(work, exist_ok=True)
    report = os.path.join(work, "report.json")
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join([mod.tools_dir(ROOT), ROOT] + [p for p in [env.get("PYTHONPATH")] if p])
    cmd = [sys.executable, "-m", mod.DEFAULT_MODULE, src, out, "--report", report]
    say("$ " + " ".join(cmd[1:3]) + " ...")
    run_logged(cmd, os.path.join(work, "lift.log"), env)
    rep = read_json(report)
    files = rep.pop("files", None) or []
    if not any(fn.endswith(".c") for fn in os.listdir(out)):
        raise Failed("%s wrote no C file" % mod.DEFAULT_MODULE)
    key = title = None
    try:
        if con == "gba":
            with open(src, "rb") as f:
                rom = f.read()
            key, title = gba.title_key(rom), gba.rom_title(rom, None)
        elif con == "ps1":
            key = ps1.read_source(src)[0]
        else:
            key, title = n64.read_source(src)[:2]
    except (OSError, ValueError):
        pass
    stem = os.path.splitext(os.path.basename(src))[0]
    with open(os.path.join(out, "report.json"), "w", encoding="utf-8") as f:
        json.dump(dict(rep, files=len(files)), f, indent=1, sort_keys=True)
        f.write("\n")
    return {"console": con, "key": key or stem, "title": title or stem, "functions": rep.get("functions"),
            "instructions": rep.get("instructions"), "interp_fallback_insns": rep.get("interp_fallback_insns")}


XBOX_README = """\
# {title}: recompiled sources (Xbox, title ID {key})

Lifted from your dump by XRecomp: the title project `python -m tools.prepare --no-build` writes. Nothing is built,
and no game file is in here: the executable needs your own dump next to it.

- toolkit: {repo}
- commit: {commit}

## What is here

- `CMakeLists.txt`, `src/main.c`, `src/recomp_manual.c`: the title project (templates/new-game)
- `src/recomp/gen/`: the lifted C ({files} files, {size} with the project's own)
- `src/title/`: the profile layers of this title, when the toolkit has some
- `images/<name>/`: the same project for each other executable of the disc that default.xbe launches
- `prepare.log`, `recomp_info.json`: how the lift went

## Build

    git clone {repo} xrecomp && git -C xrecomp checkout {commit}
    bash xrecomp/tools/linux/install_deps.sh
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DXBOXRECOMP_DIR="$PWD/xrecomp"
    cmake --build build --parallel
    ln -s /path/to/your/game.iso build/game      # or the folder of the extracted disc

The lifted C matches the runtime of that commit; with another one, lift again. On your own machine
`python3 -m tools.prepare /path/to/game.iso` does all of it in one go.
"""

CONSOLE_README = """\
# {title}: recompiled sources ({console_name}, key {key})

Lifted from your dump by XRecomp with `python -m {con}_tools.recomp`. Nothing is built, and no game file is in here.

- toolkit: {repo}
- commit: {commit}

## What is here

- `{con}_rc_*.c`: the lifted C ({files} files, {size}); `{con}_rc_table.c` is the table the runtime loads them through
- `report.json`, `recomp_info.json`: the lifter's summary

## Build

The files compile against the headers of `consoles/{con}/runtime` into the title library the runtime loads:

    git clone {repo} xrecomp && git -C xrecomp checkout {commit}
    cc -O1 -fPIC -std=gnu11 -w -DNDEBUG -I. -Ixrecomp/consoles/{con}/runtime -shared -o libtitle.so *.c
    {con}_play /path/to/your/game --pack libtitle.so

`{con}_play` is built from the toolkit (xrecomp/consoles/README.md). `xrecomp/tools/consoles/{con}_recomp_build.py`
does the compile in parallel, with the discovery loop, and `python3 -m tools.prepare /path/to/your/game` lifts,
builds and installs the pack in one go.
"""


def artifact_name(info):
    key = re.sub(r"[^A-Za-z0-9._-]+", "-", str(info["key"])).strip("-.") or "game"
    return "recomp-src-%s-%s" % (info["console"], key[:60])


def write_outputs(info, out, others):
    n, size = tree_stats(out)
    info.update(toolkit=toolkit_commit(), c_files=n, c_bytes=size)
    with open(os.path.join(out, "recomp_info.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, indent=1, sort_keys=True)
        f.write("\n")
    fields = dict(info, repo=toolkit_url(), commit=info["toolkit"], files=n, size=human(size), con=info["console"],
                  console_name=CONSOLE_NAMES[info["console"]])
    with open(os.path.join(out, "README.md"), "w", encoding="utf-8") as f:
        f.write((XBOX_README if info["console"] == "xbox" else CONSOLE_README).format(**fields))
    name = artifact_name(info)
    say("\n%s (%s, %s): %d C files, %s -> %s" % (info["title"], CONSOLE_NAMES[info["console"]], info["key"], n,
                                                 human(size), out))
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as f:
            f.write("name=%s\npath=%s\n" % (name, out))
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        rows = [("Game", info["title"]), ("Console", CONSOLE_NAMES[info["console"]]), ("Key", info["key"]),
                ("Lifted C", "%d files, %s" % (n, human(size))), ("Toolkit", info["toolkit"][:12]),
                ("Artifact", "`%s`" % name)]
        if info.get("functions") is not None:
            rows.insert(4, ("Functions", "%s (%s instructions, %s left to the interpreter)"
                            % (info["functions"], info.get("instructions"), info.get("interp_fallback_insns"))))
        health = info.get("lift_health")
        if health and any(health.values()):
            rows.insert(4, ("Lift health", ", ".join("%s %d" % (k, v) for k, v in sorted(health.items()) if v)))
        images = info.get("images") or []
        if images:
            rows.insert(4, ("Other images", ", ".join("%s%s" % (i["path"], "" if i["lifted"] else " (not lifted)")
                                                      for i in images)))
        if others:
            rows.append(("Also in the download, not lifted", ", ".join(others[:8])))
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
            f.write("### Recompiled sources\n\n| | |\n|---|---|\n")
            f.write("".join("| %s | %s |\n" % (k, str(v).replace("|", "\\|")) for k, v in rows))
            f.write("\nDownload the artifact from this run's page: the sources, `README.md` (how to build them) and "
                    "`recomp_info.json`.\n")


def cmd_lift(a):
    src, con, others = find_source(os.path.abspath(a.source))
    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or out + "-work")
    if os.path.commonpath([out, src]) == out:
        raise Failed("--out must not hold the dump")
    say("%s: %s (%s)" % (CONSOLE_NAMES[con], os.path.basename(src), human(os.path.getsize(src))))
    if others:
        say("      also there, not lifted: " + ", ".join(others[:8]))
    t0 = time.time()
    info = lift_xbox(src, out, work) if con == "xbox" else lift_console(con, src, out, work)
    info["lift_seconds"] = int(time.time() - t0)
    write_outputs(info, out, others)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog="recomp_source.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="download the dump (and unpack it when it is an archive)")
    f.add_argument("url", nargs="+")
    f.add_argument("--dest", required=True, help="the folder the dump goes to")
    f.add_argument("--name", help="the file's name, when the link gives none with an extension")
    f.set_defaults(fn=cmd_fetch)
    li = sub.add_parser("lift", help="lift the dump to C, without building")
    li.add_argument("source", help="the dump, or the folder fetch filled")
    li.add_argument("--out", required=True, help="the folder the sources go to (emptied first)")
    li.add_argument("--work", help="scratch folder (default: OUT-work)")
    li.set_defaults(fn=cmd_lift)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except Failed as e:
        print("error: %s" % e, file=sys.stderr, flush=True)
        if os.environ.get("GITHUB_ACTIONS"):
            print("::error::%s" % str(e).splitlines()[0], flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
