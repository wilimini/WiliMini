#!/bin/sh
# Host checks for the theme engine: the Lua sandbox and layout validation (tests/theme_test.cpp)
# and the shipped example themes. Lua 5.4 is built from source for the host (the PSP build uses
# the SDK's liblua); set LUA_SRC to a checkout, otherwise the pinned tag is cloned once.
set -eu
cd "$(dirname "$0")/.."
out=${1:-build/theme-test}
LUA_SRC=${LUA_SRC:-build/downloads/lua-5.4.7}
LUA_COMMIT=1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3
if [ ! -d "$LUA_SRC" ]; then
    mkdir -p "$(dirname "$LUA_SRC")"
    git clone -q --depth 1 --branch v5.4.7 https://github.com/lua/lua "$LUA_SRC"
fi
[ "$(git -C "$LUA_SRC" rev-parse HEAD 2>/dev/null || echo "$LUA_COMMIT")" = "$LUA_COMMIT" ] || { echo "unexpected Lua revision" >&2; exit 1; }
mkdir -p "$out/lua"
for f in "$LUA_SRC"/*.c; do
    b=$(basename "$f" .c)
    case $b in lua|luac|onelua|ltests) continue ;; esac
    ${CC:-cc} -std=gnu11 -O1 -w -DLUA_USE_POSIX -c "$f" -o "$out/lua/$b.o"
done
ar rcs "$out/liblua.a" "$out"/lua/*.o
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Iclient -I"$LUA_SRC" tests/theme_test.cpp client/theme.cpp \
    "$out/liblua.a" -o "$out/theme-test" -lm
"$out/theme-test"
if [ -f tests/theme_examples_test.cpp ]; then
    ${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -Iclient -I"$LUA_SRC" tests/theme_examples_test.cpp client/theme.cpp \
        "$out/liblua.a" -o "$out/theme-examples-test" -lm
    "$out/theme-examples-test" themes
fi
