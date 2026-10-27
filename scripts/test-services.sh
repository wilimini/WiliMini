#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/build/services-tests"
mkdir -p "$out"
cc -std=c11 -c "$root/client/third_party/qrcodegen/qrcodegen.c" -o "$out/qrcodegen.o"
cc -std=c11 -I"$root/decoder/include" -c "$root/client/services/http_policy.c" -o "$out/http_policy.o"
c++ -std=c++17 -Wall -Wextra -pthread \
    -I"$root/client" -I"$root/client/api/wiliwili" -I"$root/decoder/include" \
    -I"$root/third_party/nlohmann" -I"$root/third_party/pystring" \
    "$root/tests/services_test.cpp" "$root/client/api_client.cpp" \
    "$root/client/services/app_error.cpp" "$root/client/services/playback_error.cpp" \
    "$root/client/services/network.cpp" "$root/client/services/session_store.cpp" \
    "$root/client/services/community_service.cpp" "$root/client/services/auth_service.cpp" "$root/client/services/search_service.cpp" \
    "$out/qrcodegen.o" "$out/http_policy.o" -o "$out/services-test"
"$out/services-test"
