#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
out=build/host-memory
mkdir -p "$out"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Iplatform/include tests/loader_profile_test.c -o "$out/loader-profile-test"
"$out/loader-profile-test"
python3 scripts/generate-localization.py --zh-hans resources/i18n/zh-Hans.json \
    --en-us resources/i18n/en-US.json --output-dir "$out/generated"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -c client/stream_server.c -o "$out/stream_server.o"
for frames in 2 8; do
    ${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Iclient -I"$out/generated" -Ithird_party/nlohmann \
        "-DBILI_DEFAULT_RGB_FRAMES=$frames" "-DBILI_DEFAULT_RGB565=$([ "$frames" = 2 ] && echo 1 || echo 0)" tests/preferences_test.cpp \
        client/preferences.cpp client/memory_budget.cpp client/localization.cpp "$out/generated/localization.generated.cpp" \
        "$out/stream_server.o" -o "$out/preferences-$frames-test"
    "$out/preferences-$frames-test" "$out/settings-test.json" "$frames"
done
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Iclient tests/tile_layout_test.cpp -o "$out/tile-layout-test"
"$out/tile-layout-test"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Iclient tests/comment_layout_test.cpp -o "$out/comment-layout-test"
"$out/comment-layout-test"
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -Itests/psp_stubs -Iplatform/include tests/rgb_pacer_test.c \
    platform/src/wili_log.c -o "$out/rgb-pacer-test"
"$out/rgb-pacer-test"
