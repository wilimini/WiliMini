#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PSPDEV=${PSPDEV:-/Users/liu/pspdev}
export PATH="$PSPDEV/bin:$PATH"
out="${BILI_FFMPEG_BUILD_DIR:-$root/build/ffmpeg-audio}"
install_prefix="${PSP_FFMPEG_PREFIX:-$root/build/ffmpeg-audio-install}"
# third_party/ffmpeg is an external checkout; apply the PSP changes reproducibly.
apply_ffmpeg_patch() {
    patch_file=$1
    if ! patch -d "$root/third_party/ffmpeg" -p1 -R --dry-run < "$patch_file" >/dev/null 2>&1; then
        patch -d "$root/third_party/ffmpeg" -p1 --forward < "$patch_file"
    fi
}

apply_ffmpeg_patch "$root/patches/ffmpeg/0001-psp-sampled-h264-profile.patch"
apply_ffmpeg_patch "$root/patches/ffmpeg-psp-tx-tables.patch"
mkdir -p "$out"
cd "$out"
    "$root/third_party/ffmpeg/configure" \
        --prefix="$install_prefix" \
        --enable-cross-compile --target-os=none --arch=mips --cpu=allegrex \
        --cross-prefix=psp- --disable-asm --disable-autodetect \
        --disable-programs --disable-doc --disable-debug --disable-network \
        --disable-avdevice --disable-avfilter --disable-postproc \
        --disable-swscale --disable-swresample --disable-pthreads \
        --disable-everything --enable-decoder=h264,aac,aac_fixed --enable-parser=h264 \
        --enable-demuxer=mov,flv --enable-protocol=file \
        --extra-cflags='-O3 -G0 -Wno-error=incompatible-pointer-types' --extra-ldflags='-G0' \
        --pkg-config=/usr/bin/false
make -j4
make install
