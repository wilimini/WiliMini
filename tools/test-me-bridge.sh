#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
mkdir -p build/host-me-bridge
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -Itests/me_bridge_stubs \
    tests/me_bridge_test.c -o build/host-me-bridge/test
build/host-me-bridge/test
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -fsanitize=address,undefined -Itests/me_bridge_stubs \
    tests/custom_core_bridge_test.c -o build/host-me-bridge/custom-core-test
build/host-me-bridge/custom-core-test
