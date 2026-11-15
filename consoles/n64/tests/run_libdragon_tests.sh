#!/bin/sh
# ctest helper (XBA-202): run_libdragon_tests.sh N64_RUN ROM KNOWN [n64_run args...]. Runs libdragon's test ROM
# (testrom_emu.z64, tools/consoles/build_n64_libdragon_tests.sh) under n64_run (the extra arguments, e.g. --pack LIB),
# reads its IS-Viewer text: "**** Starting test: NAME" per test, "ASSERTION FAILED" in a failing one, and the summary
# "Passed: N out of M (K skipped)". Passes when the suite finished, every failing test is in KNOWN (test names, one
# per line; # comments) and N reaches N64_LIBDRAGON_MIN_PASS (env, when set). Prints the counts and the failures.
# Exits 77 when the ROM is not built.
run=$1
rom=$2
known=$3
shift 3
if [ ! -f "$rom" ]; then
    echo "skipped: $rom not built (tools/consoles/build_n64_libdragon_tests.sh)"
    exit 77
fi
out=$(timeout 600 "$run" "$rom" --tty --frames 6000 --quiet "$@" 2>&1)
summary=$(printf '%s\n' "$out" | grep -E "^Passed: [0-9]+ out of [0-9]+" | tail -n 1)
if [ -z "$summary" ]; then
    printf '%s\n' "$out" | grep -v "^[0-9a-f]\{8\} " | tail -n 30
    echo "FAIL: the libdragon test ROM did not finish"
    exit 1
fi
echo "$summary"
passed=$(echo "$summary" | sed 's/^Passed: \([0-9]*\) out of \([0-9]*\) (\([0-9]*\) skipped).*/\1/')
total=$(echo "$summary" | sed 's/^Passed: \([0-9]*\) out of \([0-9]*\) (\([0-9]*\) skipped).*/\2/')
skipped=$(echo "$summary" | sed 's/^Passed: \([0-9]*\) out of \([0-9]*\) (\([0-9]*\) skipped).*/\3/')
failing=$(printf '%s\n' "$out" | awk '/^\*\*\*\* Starting test: /{name=$4} /^ASSERTION FAILED/{print name}' | sort -u)
nfail=$((total - passed - skipped))
rc=0
bad=""
for name in $failing; do
    if grep -v '^#' "$known" | grep -qx "$name"; then
        echo "known failure: $name"
    else
        bad="$bad $name"
    fi
done
if [ -n "$bad" ]; then
    echo "FAIL: unexpected failures:$bad"
    printf '%s\n' "$out" | grep -A4 "^ASSERTION FAILED" | grep -v "^[0-9a-f]\{8\} " | head -n 40
    rc=1
fi
if [ "$(echo "$failing" | grep -c .)" -ne "$nfail" ]; then
    echo "FAIL: $nfail failing tests, $(echo "$failing" | grep -c .) with an assertion"
    rc=1
fi
if [ -n "$N64_LIBDRAGON_MIN_PASS" ] && [ "$passed" -lt "$N64_LIBDRAGON_MIN_PASS" ]; then
    echo "FAIL: $passed passing, the threshold is $N64_LIBDRAGON_MIN_PASS"
    rc=1
fi
[ "$rc" = 0 ] && echo "libdragon tests: OK ($passed passed, $nfail known failures, $skipped skipped)"
exit $rc
