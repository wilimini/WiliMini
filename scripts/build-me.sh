#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PSPDEV=${PSPDEV:-/Users/liu/pspdev}
export PATH="$PSPDEV/bin:$PATH"
sdk=$(psp-config --pspsdk-path)
out="$root/build/me"
mist="$root/third_party/psp-media-engine-mist"
core="$root/third_party/psp-media-engine-custom-core"
test "$(git -C "$mist" rev-parse HEAD)" = 30bbdc204e53c7fbda8229fcebad305c07115f05
test "$(git -C "$core" rev-parse HEAD)" = 0197c99ea581d728188af30ccfd9bb5f7afe6280
mkdir -p "$out/include" "$root/results/psp-me-full"
ln -sfn "$core" "$out/include/me-core-mapper"
cp "$root"/experiments/me/* "$root"/decoder/me/* "$out/"
psp-build-exports -b "$out/exports.exp" > "$out/exports.c"
cp "$root/decoder/me/me_lab.S" "$out/me_lab.S"
for source in "$out/bridge.c" "$out/exports.c" "$mist/me-mist.c" "$mist/me-mist-utils.c" "$core/me-core-mapping.c"; do
    psp-gcc -c -O2 -G0 -Wall -ffunction-sections -fdata-sections \
        -I"$out" -I"$out/include" -I"$mist" -I"$root/decoder/src" -I"$sdk/include" \
        "$source" -o "$out/$(basename "$source" .c).o"
done
psp-gcc -nostartfiles -Wl,-q,-T"$sdk/lib/linkfile.prx",-zmax-page-size=128 \
    -G0 -L"$sdk/lib" \
    "$out/bridge.o" "$out/exports.o" "$out/me-mist.o" "$out/me-mist-utils.o" "$out/me-core-mapping.o" \
    -lpspkernel -lpspsdk -o "$out/me_bridge.elf"
psp-fixup-imports "$out/me_bridge.elf"
psp-prxgen "$out/me_bridge.elf" "$out/me_bridge.prx"
make -B -C "$out" -j2 INCDIR="$out/include $root/decoder/src . $PSPDEV/psp/include $sdk/include"
