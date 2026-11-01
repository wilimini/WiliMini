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
# Run folder naming, numbering and pruning rules (pure logic; the directory calls are PSP-only).
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Iplatform/include tests/wili_results_test.c -o "$out/wili-results-test"
"$out/wili-results-test"
# Streaming sink: writer thread, rotation, bursts. Short intervals keep the test quick; run it
# under AddressSanitizer+UBSan and again under ThreadSanitizer to catch lock and ordering bugs.
sink_flags="-DWILI_LOG_SINK_FLUSH_MS=300 -DWILI_LOG_SINK_IDLE_MS=200"
for sanitizer in address,undefined thread; do
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -pthread -fsanitize=$sanitizer $sink_flags \
        -Iplatform/include -Iplatform/src platform/src/wili_log.c platform/src/wili_log_sink.c \
        tests/wili_log_sink_test.c -o "$out/wili-log-sink-test"
    "$out/wili-log-sink-test"
done
