#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
source=${PPSSPP_SOURCE:-"$root/build/ppsspp-src"}
build=${PPSSPP_BUILD_DIR:-"$root/build/ppsspp-headless-src"}
out=${PPSSPP_HEADLESS_DIR:-"$root/build/ppsspp-headless"}

if [ ! -f "$source/CMakeLists.txt" ]; then
    echo "Set PPSSPP_SOURCE to a PPSSPP checkout with its required submodules initialized." >&2
    exit 1
fi

cmake -S "$source" -B "$build" \
    -DHEADLESS=ON \
    -DHEADLESS_CROSS=ON \
    -DUSE_FFMPEG=ON \
    -DBUILD_BUNDLED_FFMPEG=ON \
    -DUSE_DISCORD=OFF \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --target PPSSPPHeadless --parallel "${JOBS:-4}"
mkdir -p "$out/assets"
cp "$build/PPSSPPHeadless" "$out/PPSSPPHeadless"
cp -R "$build/assets/." "$out/assets/"
