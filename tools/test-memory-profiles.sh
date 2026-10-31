#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
out=build/host-memory
mkdir -p "$out"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Iplatform/include tests/loader_profile_test.c -o "$out/loader-profile-test"
"$out/loader-profile-test"
for frames in 2 8; do
    ${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Iclient -Ithird_party/nlohmann \
        "-DBILI_DEFAULT_RGB_FRAMES=$frames" tests/preferences_test.cpp \
        client/preferences.cpp client/memory_budget.cpp -o "$out/preferences-$frames-test"
    "$out/preferences-$frames-test" "$out/settings-test.json" "$frames"
done
${CC:-cc} -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -pthread \
    -fsanitize=address,undefined -Itests/psp_stubs -Iplatform/include tests/rgb_pacer_test.c \
    platform/src/wili_log.c -o "$out/rgb-pacer-test"
"$out/rgb-pacer-test"
