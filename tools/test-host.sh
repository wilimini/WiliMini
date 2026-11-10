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
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror \
    tests/music_queue_test.cpp client/services/music_queue.cpp -o "$out/music-queue-test"
"$out/music-queue-test"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tests/pinyin_ime_test.cpp client/pinyin_ime.cpp -o "$out/pinyin-ime-test"
"$out/pinyin-ime-test" client/assets/ime/pinyin.dat
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tests/bitmap_font_test.cpp client/bitmap_font.cpp -o "$out/bitmap-font-test"
"$out/bitmap-font-test" client/assets/font/wqy.bmf
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Ithird_party/nlohmann \
    tests/update_core_test.cpp client/services/update_core.cpp -o "$out/update-core-test"
"$out/update-core-test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    tests/http_buffer_test.c -o "$out/http-buffer-test"
"$out/http-buffer-test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror tests/playback_controls_test.c -o "$out/playback-controls-test"
"$out/playback-controls-test"
sh tools/test-update-package.sh
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iplatform/include \
    tests/update_swap_test.c -o "$out/update-swap-test" # includes the library source itself
"$out/update-swap-test"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iplatform/include \
    tests/update_swap_posix_test.c platform/src/wili_update_swap.c -o "$out/update-swap-posix-test"
"$out/update-swap-posix-test"
echo "All host checks passed"
