#!/bin/sh
# Host check for update_core / update_package (needs zlib headers and python3).
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
out=build/host-tests
mkdir -p "$out"
python3 -I - "$out/update-fixture.zip" <<'PY'
import sys, zipfile
p = sys.argv[1]
with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr(zipfile.ZipInfo("WiliMini/"), "")
    z.writestr(zipfile.ZipInfo("WiliMini/EBOOT.PBP"), "eboot", compress_type=zipfile.ZIP_STORED)
    z.writestr("WiliMini/empty", "")
    z.writestr("WiliMini/big.bin", "x" * 300000)
with zipfile.ZipFile(p + ".evil", "w") as z:
    z.writestr("WiliMini/../../evil", "x")
PY
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Ithird_party/nlohmann \
    tests/update_package_test.cpp client/services/update_package.cpp client/services/update_core.cpp \
    -lz -o "$out/update-package-test"
"$out/update-package-test" "$out/update-fixture.zip"
