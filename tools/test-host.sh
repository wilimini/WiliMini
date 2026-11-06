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
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tests/pinyin_ime_test.cpp client/pinyin_ime.cpp -o "$out/pinyin-ime-test"
"$out/pinyin-ime-test" client/assets/ime/pinyin.dat
${CC:-cc} -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    tests/stream_server_test.c client/stream_server.c -o "$out/stream-server-test"
"$out/stream-server-test" tests/data
if command -v go >/dev/null 2>&1 && [ -f server/go.mod ]; then
    (cd server && go vet ./... && go test ./...)
else
    echo "go not installed: skipping the wili-server tests"
fi
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tests/http_buffer_test.c -o "$out/http-buffer-test"
"$out/http-buffer-test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror tests/psmf_header_test.c -o "$out/psmf-header-test"
"$out/psmf-header-test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror tests/sce_mpeg_options_test.c -o "$out/sce-mpeg-options-test"
"$out/sce-mpeg-options-test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror tests/playback_controls_test.c -o "$out/playback-controls-test"
"$out/playback-controls-test"
echo "All host checks passed"
