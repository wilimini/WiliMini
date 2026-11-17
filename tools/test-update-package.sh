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
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Ithird_party/nlohmann \
    tests/update_package_test.cpp client/services/update_package.cpp client/services/update_core.cpp \
    -lz -o "$out/update-package-test"
"$out/update-package-test" "$out/update-fixture.zip"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iplatform/include \
    -c platform/src/wili_update_swap.c -o "$out/wili_update_swap.o"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Ithird_party/nlohmann -Iplatform/include \
    tests/update_install_test.cpp client/services/update_install.cpp client/services/update_package.cpp \
    client/services/update_core.cpp "$out/wili_update_swap.o" -lz -o "$out/update-install-test"
"$out/update-install-test" "$out/update-fixture.zip.pkg"
${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -Ithird_party/nlohmann -Iplatform/include \
    tests/update_controller_test.cpp client/services/update_controller.cpp client/services/update_install.cpp \
    client/services/update_package.cpp client/services/update_core.cpp "$out/wili_update_swap.o" -lz -lpthread \
    -o "$out/update-controller-test"
"$out/update-controller-test" "$out/update-fixture.zip.pkg"
# Multi-connection download for several connection counts, with ASan/UBSan and with TSan.
for n in 1 2 3 5 8; do
    for san in address,undefined thread; do
        ${CC:-cc} -std=c11 -Wall -Wextra -Werror -fsanitize=$san -Iplatform/include \
            -c platform/src/wili_update_swap.c -o "$out/wili_update_swap_par.o"
        ${CXX:-c++} -std=c++17 -Wall -Wextra -Werror -fsanitize=$san -DBILI_UPDATE_CONNECTIONS=$n \
            -Ithird_party/nlohmann -Iplatform/include tests/update_parallel_test.cpp client/services/update_install.cpp \
            client/services/update_package.cpp client/services/update_core.cpp "$out/wili_update_swap_par.o" -lz -lpthread \
            -o "$out/update-parallel-test"
        "$out/update-parallel-test"
    done
done
