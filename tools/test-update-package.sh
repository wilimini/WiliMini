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
with zipfile.ZipFile(p + ".pkg", "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("WiliMini/EBOOT.PBP", "new-eboot")
    z.writestr("WiliMini/wili_mini.prx", "new-prx")
    import os
    z.writestr("WiliMini/lib/extra.bin", os.urandom(20000))
with zipfile.ZipFile(p + ".evil", "w") as z:
    z.writestr("WiliMini/../../evil", "x")
PY
for f in wili_update_swap wili_update_swap_run; do
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iplatform/include \
        -c platform/src/$f.c -o "$out/$f.o"
done
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -c client/json.c -o "$out/json.o"
C="$out/wili_update_swap.o $out/wili_update_swap_run.o $out/json.o"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Ithird_party/nlohmann \
    tests/update_package_test.cpp client/services/update_package.cpp client/services/update_core.cpp \
    $C -lz -o "$out/update-package-test"
"$out/update-package-test" "$out/update-fixture.zip"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Ithird_party/nlohmann -Iplatform/include \
    tests/update_install_test.cpp client/services/update_install.cpp client/services/update_package.cpp \
    client/services/update_core.cpp $C -lz -o "$out/update-install-test"
"$out/update-install-test" "$out/update-fixture.zip.pkg"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Ithird_party/nlohmann -Iplatform/include \
    tests/update_controller_test.cpp client/services/update_controller.cpp client/services/update_install.cpp \
    client/services/update_package.cpp client/services/update_core.cpp $C -lz -lpthread \
    -o "$out/update-controller-test"
"$out/update-controller-test" "$out/update-fixture.zip.pkg"
