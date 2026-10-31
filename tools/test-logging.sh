#!/bin/sh
# Host check for platform/src/wili_log.c: line format, filtering, spec parsing,
# ring buffer, concurrent writers and truncation. Does not exercise the PSP build.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
out=build/host-logging
mkdir -p "$out"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -Iplatform/include platform/src/wili_log.c tests/wili_log_test.c -o "$out/wili-log-test"
"$out/wili-log-test"
# The loader links the logger without a ring buffer; it must still compile cleanly.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -DWILI_LOG_RING_LINES=0 -Iplatform/include \
    -c platform/src/wili_log.c -o "$out/wili-log-noring.o"
