#!/bin/sh
# Runs every host-side check (no PSP toolchain or emulator needed).
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
out=build/host-tests
mkdir -p "$out"
sh tools/test-memory-profiles.sh
sh tools/test-logging.sh
sh tools/test-me-bridge.sh
python3 tools/test-http-transport.py
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Ithird_party/nlohmann \
    tests/danmaku_test.cpp client/services/danmaku.cpp -o "$out/danmaku-test"
"$out/danmaku-test"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Ithird_party/nlohmann \
    tests/subtitles_test.cpp client/services/subtitles.cpp -o "$out/subtitles-test"
"$out/subtitles-test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror tests/playback_controls_test.c -o "$out/playback-controls-test"
"$out/playback-controls-test"
echo "All host checks passed"
