#!/bin/sh
# Build PSPLink from the pinned third_party/psplinkusb submodule with
# patches/psplink-buttons.patch applied (adds the "btn" shell command).
# The submodule itself is left untouched; the build runs in a copy.
# Usage: build-psplink.sh [build-dir]   (default build/psplink)
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/third_party/psplinkusb"
OUT=${1:-"$ROOT/build/psplink"}

if [ ! -e "$SRC/Makefile.psp" ]; then
    echo "Missing $SRC; run: git submodule update --init third_party/psplinkusb" >&2
    exit 1
fi
command -v psp-config >/dev/null 2>&1 || {
    echo "psp-config not found; set PSPDEV and add \$PSPDEV/bin to PATH." >&2
    exit 1
}

rm -rf "$OUT"
mkdir -p "$OUT"
(cd "$SRC" && git archive HEAD) | tar -x -C "$OUT"
patch -d "$OUT" -p1 --forward --batch < "$ROOT/patches/psplink-buttons.patch"
make -C "$OUT" -f Makefile.psp

echo "PSPLink built: $OUT/psplink/psplink.prx"
echo "Boot with:    $OUT/bootstrap/EBOOT.PBP (plus psplink.ini, psplink_user.prx, usbhostfs.prx)"
