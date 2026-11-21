#!/bin/sh
# Host tests for the pure-logic parts (no Vita, no game files needed). Run inside the vitasdk container from the repository folder:
#   docker run --rm -v "$(pwd):/src" -w /src vitasdk/vitasdk sh tests/run_host_tests.sh
fail=0
mkdir -p build/host
t() {
    name=$1; shift
    echo "== $name"
    if gcc -std=c11 -O1 -Iloader -o "build/host/$name" "$@" 2> "build/host/$name.build.txt"; then
        "./build/host/$name" > "build/host/$name.txt" 2>&1 || fail=1
        grep -E "FAIL|PASSED|ALL CHECKS" "build/host/$name.txt" | tail -3
    else
        echo "BUILD FAILED"; cat "build/host/$name.build.txt" | head -20; fail=1
    fi
}
t offline tests/host_offline_test.c loader/offline_b.c loader/bmsg.c loader/bytestream.c loader/passdata.c loader/profile.c loader/botnames.c loader/botworld.c
t profile tests/host_profile_test.c loader/profile.c
t maprot tests/host_maprot_test.c loader/maprot.c
t csvpatch tests/host_csvpatch_test.c loader/csvpatch.c loader/maprot.c
t offertitles tests/host_offertitles_test.c loader/offertitles.c
t passdata tests/host_passdata_test.c loader/passdata.c
t vpad tests/host_vpad_test.c loader/vpad.c
t botnames tests/host_botnames_test.c loader/botnames.c
t botworld tests/host_botworld_test.c loader/botworld.c loader/bytestream.c
t touch tests/host_touch_test.c loader/touch.c
echo "RESULT fail=$fail"
[ "$fail" = 0 ]
