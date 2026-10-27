#!/bin/sh
# Test the production PSP select adapter with native socket IDs above FD_SETSIZE.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/wili-mini-select.XXXXXX")
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
"${CC:-cc}" -O1 -g -fsanitize=address,undefined "$root/tests/psp_select_test.c" -o "$test_dir/test"
"$test_dir/test"
