#!/bin/sh
# Copy a packaged WiliMini directory to the PSP memory card through PSPLink.
# usbhostfs_pc must already be running with the repository root as host0:, and
# the PSP must be in PSPLink. This script only runs pspsh; it never starts usbhostfs_pc.
# Usage: psplink-install.sh PACKAGE_DIR DEST_DIR [PORT]
#   e.g. psplink-install.sh dist/PSP/GAME/WiliMini ms0:/PSP/GAME/WiliMini 11000
set -eu
pkg=${1:?package dir (relative to the usbhostfs root)}
dest=${2:?destination, e.g. ms0:/PSP/GAME/WiliMini}
port=${3:-11000}
[ -f "$pkg/EBOOT.PBP" ] || { echo "Missing $pkg/EBOOT.PBP; run make package first." >&2; exit 1; }
script=$(mktemp)
trap 'rm -f "$script"' EXIT
pkg=${pkg%/}
dest=${dest%/}
# PSPLink's mkdir fails (and aborts a pspsh script) when the directory exists,
# so each mkdir runs on its own and its failure is ignored.
{
    echo "${dest%/*}"
    echo "$dest"
    (cd "$pkg" && find . -mindepth 1 -type d | sort | sed 's|^\./||') | while read -r d; do
        echo "$dest/$d"
    done
} | while read -r d; do
    pspsh -p "$port" -e "mkdir $d" >/dev/null 2>&1 || true
done
{
    (cd "$pkg" && find . -type f | sort | sed 's|^\./||') | while read -r f; do
        echo "cp host0:/$pkg/$f $dest/$f"
    done
} > "$script"
echo "Copying $pkg -> $dest through pspsh on port $port"
pspsh -p "$port" "$script"
