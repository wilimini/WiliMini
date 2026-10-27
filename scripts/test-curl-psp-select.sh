#!/bin/sh
# Exercise the real patched Curl_poll with injected select results on the host.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_dir="$root/build/curl-source"
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/bili-curl-select.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
printf '%s\n' 'int sceKernelDelayThread(unsigned int);' > "$test_dir/pspthreadman.h"
case $(uname -s) in
    Darwin) gc_flag=-Wl,-dead_strip ;;
    *) gc_flag=-Wl,--gc-sections ;;
esac
"${CC:-cc}" -O2 -ffunction-sections -fdata-sections -DHAVE_CONFIG_H -D__PSP__ \
    -Dselect=test_select -I"$test_dir" -I"$source_dir/lib" -I"$source_dir/include" \
    -c "$source_dir/lib/select.c" -o "$test_dir/select.o"
"${CC:-cc}" -O2 -DHAVE_CONFIG_H -I"$source_dir/lib" -I"$source_dir/include" \
    "$root/tests/curl_psp_select_test.c" "$test_dir/select.o" "$gc_flag" -o "$test_dir/test"
"$test_dir/test"
