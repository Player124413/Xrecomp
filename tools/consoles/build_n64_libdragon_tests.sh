#!/usr/bin/env bash
# Builds the libdragon-based open test ROMs from source, for the N64 core's tests (consoles/n64/tests). XBA-202.
#   - libdragon's test ROM (DragonMinded/libdragon tests/, Unlicense: public domain): n64_libdragon_tests
#   - sauraen/rsp_inval_instrs (Unlicense), the RSP's invalid opcodes as measured on hardware: n64_rsp_inval_instrs
#
#   tools/consoles/build_n64_libdragon_tests.sh [OUTDIR]     (default build/n64-libdragon-tests -> OUTDIR/testrom_emu.z64,
#                                                             OUTDIR/rsp_inval_instrs.z64)
#
# libdragon is fetched at the commit tools/docker/n64-homebrew.Dockerfile pins (LIBDRAGON_COMMIT: the library the
# toolchain image has installed) into OUTDIR/src, a cache kept between runs, then tests/ is built in a throwaway
# container of that image (tag xba-n64-homebrew, built by build_homebrew.sh, or here), without network:
# testrom_emu.z64 is the suite with IN_EMULATOR=1 (it skips the tests libdragon marks as not runnable on emulators,
# e.g. exact timings), and prints each test and the summary ("Passed: N out of M (K skipped)") to the IS-Viewer,
# which n64_run --tty shows. Open boot code (libdragon's IPL3), no Nintendo code; the output is never committed
# (build/ is ignored). rsp_inval_instrs is fetched at RSP_INVAL_COMMIT below into OUTDIR/rsp_inval_instrs-src and
# built the same way; it prints "Test passed" / "Test failed" to the IS-Viewer. Sources go in and the ROMs come out
# with `docker cp` (no bind mounts).
#
# Env: JOBS (default 4: the machine is shared), XBA_HOMEBREW_TIMEOUT (seconds per docker step, default 1800),
# DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

out=${1:-build/n64-libdragon-tests}
jobs=${JOBS:-4}
tmo=${XBA_HOMEBREW_TIMEOUT:-1800}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-n64-homebrew
dockerfile=$repo/tools/docker/n64-homebrew.Dockerfile
rsp_inval_commit=25b99a46714924752ee844e900ce57b51c33a4a5 # RSP_INVAL_COMMIT
commit=$(sed -n 's/^ARG LIBDRAGON_COMMIT=\([0-9a-f]\{40\}\)$/\1/p' "$dockerfile")
[ -n "$commit" ] || {
    echo "no LIBDRAGON_COMMIT in $dockerfile" >&2
    exit 1
}
command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
mkdir -p "$out"
out=$(cd "$out" && pwd)

# fetch URL COMMIT DIR: DIR holds URL at COMMIT (kept when it already does).
fetch() {
    if [ "$(git -C "$3" rev-parse HEAD 2>/dev/null || true)" != "$2" ]; then
        echo "== fetching $1 $2"
        rm -rf "$3"
        mkdir -p "$3"
        git -C "$3" init -q
        timeout "$tmo" git -C "$3" fetch -q --depth 1 "$1" "$2"
        git -C "$3" checkout -q FETCH_HEAD
        test "$(git -C "$3" rev-parse HEAD)" = "$2"
    fi
}
src=$out/src
fetch https://github.com/DragonMinded/libdragon.git "$commit" "$src"
fetch https://github.com/sauraen/rsp_inval_instrs.git "$rsp_inval_commit" "$out/rsp_inval_instrs-src"

echo "== image $image"
# shellcheck disable=SC2086
timeout "$tmo" docker build ${DOCKER_BUILD_ARGS:-} --build-arg JOBS="$jobs" -f "$dockerfile" -t "$image" \
    "$repo/tools/docker"

cid=$(docker create --network none -e JOBS="$jobs" "$image" bash -c \
    '{ cd /work/libdragon/tests && make -j"$JOBS" testrom_emu.z64 && cd /work/rsp_inval_instrs && make -j"$JOBS"; } \
        >/tmp/build.log 2>&1 || { tail -n 30 /tmp/build.log; exit 1; }')
trap 'docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
tar -C "$src" --exclude-vcs --transform 's,^\.,libdragon,' -cf - . | docker cp - "$cid:/work"
tar -C "$out/rsp_inval_instrs-src" --exclude-vcs --transform 's,^\.,rsp_inval_instrs,' -cf - . | docker cp - "$cid:/work"
timeout "$tmo" docker start -a "$cid"
docker cp "$cid:/work/libdragon/tests/testrom_emu.z64" "$out/testrom_emu.z64"
docker cp "$cid:/work/rsp_inval_instrs/rsp_inval_instrs.z64" "$out/rsp_inval_instrs.z64"
cp "$src/LICENSE.md" "$out/LICENSE.md"
cp "$out/rsp_inval_instrs-src/LICENSE.md" "$out/LICENSE-rsp_inval_instrs.md"
printf '%s\n' "DragonMinded/libdragon $commit tests/testrom_emu.z64 (Unlicense)" \
    "sauraen/rsp_inval_instrs $rsp_inval_commit rsp_inval_instrs.z64 (Unlicense)" >"$out/VERSION"
echo "== $out: $(cat "$out/VERSION")"
