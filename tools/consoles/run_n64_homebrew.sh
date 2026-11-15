#!/usr/bin/env bash
# The N64 homebrew corpus through the runtime's ctest, in Docker (CI: the homebrew job).
#
#   tools/consoles/run_n64_homebrew.sh [CORPUS_DIR] [OUT_DIR] [OPEN_DIR]   (default: build/homebrew build/n64-homebrew)
#
# CORPUS_DIR is what tools/consoles/build_homebrew.sh wrote (CORPUS_DIR/n64/<prog>.z64 + .elf, our own programs). The
# linux job has no corpus, so there every N64 ROM test skips (77); this runs them for real. In a throwaway container
# of the linux job's dependency stage (tools/docker/linux.Dockerfile --target deps, tag xba-linux-deps, shared with
# run_gba_packs.sh), without network: consoles/n64/tests built with N64_HOMEBREW_DIR pointing at the corpus, then
#   ctest -R '^n64_(runtime|pack|(rc_)?hb_|(rc_)?state_|rewind_hb_|cheats_hb_)'
# the runtime's headless runs of gfx.z64, the pack identity, each program under n64_run --result (interpreter) and
# recompiled by the discovery loop in lockstep with the interpreter, the save-state and rewind round trips, the
# GameShark codes. A test that skips although the corpus is here fails the run. n64-systemtest (n64_systemtest /
# n64_rc_systemtest) is not built here: it is not part of the corpus.
# OPEN_DIR (optional, XBA-202): what tools/consoles/build_n64_libdragon_tests.sh wrote (libdragon's test ROM and
# rsp_inval_instrs): then n64_libdragon_tests (with its pass-count threshold) and n64_rsp_inval_instrs run too.
# Sources go in and results come out with `docker cp` (no bind mounts: CI drives the host's Docker from a container).
# OUT_DIR gets build.log and ctest.log. Exits 1 when a test fails or skips.
#
# Env: JOBS (default 4: the machine is shared), XBA_N64_HB_TIMEOUT (seconds for the image build and for the run,
# default 1800), DOCKER_BUILD_ARGS (extra `docker build` flags).
set -euo pipefail

corpus=${1:-build/homebrew}
out=${2:-build/n64-homebrew}
open=${3:-}
jobs=${JOBS:-4}
tmo=${XBA_N64_HB_TIMEOUT:-1800}
repo=$(cd "$(dirname "$0")/../.." && pwd)
image=xba-linux-deps

command -v docker >/dev/null || {
    echo "docker is required" >&2
    exit 1
}
ls "$corpus"/n64/*.z64 >/dev/null 2>&1 || {
    echo "$corpus/n64 has no .z64: run tools/consoles/build_homebrew.sh n64 $corpus first" >&2
    exit 1
}

echo "== image $image (linux.Dockerfile, stage deps)"
start=$(date +%s)
# No build context: the deps stage copies nothing.
# shellcheck disable=SC2086
timeout "$tmo" docker build ${DOCKER_BUILD_ARGS:-} --target deps -t "$image" - <"$repo/tools/docker/linux.Dockerfile"
echo "== image ready in $(($(date +%s) - start)) s"

cid=
cleanup() { [ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true; }
trap cleanup EXIT

# shellcheck disable=SC2016
cmd='mkdir -p /out && cd /usr/src
export SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy
cmake -S consoles/n64/tests -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DN64_HOMEBREW_DIR=/usr/src/hb \
    -DN64_LIBDRAGON_TESTROM=/usr/src/open/testrom_emu.z64 >/out/build.log 2>&1 && cmake --build /tmp/build -j"$JOBS" >>/out/build.log 2>&1 \
  || { tail -n 30 /out/build.log; exit 1; }
rc=0
t0=$(date +%s)
echo "== ctest n64 runtime / hb / rc_hb / state / rewind (interpreter, discovery-loop recompilation in lockstep)"
re="^n64_(runtime|pack|(rc_)?hb_|(rc_)?state_|rewind_hb_|cheats_hb_)"
[ -f /usr/src/open/testrom_emu.z64 ] && re="$re|^n64_(libdragon_tests|rsp_inval_instrs)\$"
ctest --test-dir /tmp/build -R "$re" -j"$JOBS" --output-on-failure \
    --timeout 900 >/out/ctest.log 2>&1 || rc=1
grep -E "^ *[0-9]+/[0-9]+ Test|tests passed|tests failed" /out/ctest.log
grep -q "Skipped" /out/ctest.log && { echo "a test skipped although the corpus is here"; rc=1; }
[ "$rc" = 0 ] || grep -A30 "The following tests FAILED" /out/ctest.log || true
echo "== N64 ctest in $(($(date +%s) - t0)) s"
exit $rc'

cid=$(docker create --network none -e JOBS="$jobs" "$image" bash -c "$cmd")
tar -C "$repo" --exclude=__pycache__ -cf - consoles tools src | docker cp - "$cid:/usr/src"
docker cp "$corpus/n64" "$cid:/usr/src/hb"
[ -z "$open" ] || tar -C "$open" --transform 's,^,open/,' -cf - testrom_emu.z64 rsp_inval_instrs.z64 |
    docker cp - "$cid:/usr/src"
rc=0
timeout "$tmo" docker start -a "$cid" || rc=$?
rm -rf "$out"
mkdir -p "$out"
docker cp "$cid:/out/." "$out" || true
docker rm "$cid" >/dev/null
cid=
echo "== results in $out"
ls "$out"
exit "$rc"
