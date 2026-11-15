#!/bin/sh
# ctest helper (XBA-202): recomp_libdragon_tests.sh N64_RUN ROM KNOWN WORKDIR
#
# libdragon's test ROM recompiled: its pack is built with the discovery loop (tools/consoles/n64_recomp_build.py; the
# suite loads code at run time, its dl tests, so the loop need not converge: the last pack has every record), then
# the suite runs under n64_run --pack (run_libdragon_tests.sh: the interpreter's pass / known-failure set and
# threshold must come out), and once more in lockstep with the interpreter (the whole state compared after every
# frame). Exits 77 when the ROM is not built (tools/consoles/build_n64_libdragon_tests.sh) or python3 / cc are missing.
run=$1
rom=$2
known=$3
work=$4
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built (tools/consoles/build_n64_libdragon_tests.sh)"
    exit 77
fi
py=${PYTHON:-python3}
command -v "$py" >/dev/null 2>&1 || { echo "skipped: no $py"; exit 77; }
command -v "${CC:-cc}" >/dev/null 2>&1 || { echo "skipped: no C compiler"; exit 77; }
if [ ! -f "$work/libtitle.so" ] || [ "$rom" -nt "$work/libtitle.so" ] || [ -n "$N64_RC_REBUILD" ]; then
    rm -rf "$work"
    timeout 3000 "$py" "$repo/tools/consoles/n64_recomp_build.py" "$rom" "$work" --count --loop 4 --n64-run "$run" \
        --frames 6000 | tail -n 3
    [ -f "$work/libtitle.so" ] || exit 1
fi
echo "recompiled:"
sh "$here/run_libdragon_tests.sh" "$run" "$rom" "$known" --pack "$work/libtitle.so" || exit 1
echo "lockstep with the interpreter:"
out=$(timeout 900 "$run" "$rom" --frames 6000 --quiet --pack "$work/libtitle.so" --lockstep 2>&1)
echo "$out" | grep -E "^LOCKSTEP"
echo "$out" | grep -q "^LOCKSTEP OK" || exit 1
exit 0
