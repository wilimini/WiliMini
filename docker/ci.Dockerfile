# syntax=docker/dockerfile:1.7
# CI/dev environment image: tools and prebuilt third-party pieces only. It never
# builds or tests WiliMini; jobs run inside it (`container:` in GitHub Actions, or
# `docker run -v "$PWD":/work`) and build the package from the checked-out repo.
#
#   docker build -f docker/ci.Dockerfile --target ci -t wilimini-ci .
#   docker build -f docker/ci.Dockerfile --target ci-playback -t wilimini-ci-playback .
#
# Targets:
#   ci           toolchain + prebuilt PSP FFmpeg/libcurl. No Sony firmware: may be public.
#   ci-playback  ci + PPSSPP headless + ME firmware, for the PPSSPP replay tests.
#                Contains Sony firmware: keep it in a PRIVATE registry package.
#
# Build context is the repo root, trimmed by .dockerignore to the few files below, so an
# image rebuilds only when those files (or the pins) change.

ARG UBUNTU_VERSION=24.04
# Keep PSPDEV_VERSION and PPSSPP_REF here only; workflows read the built image.
ARG PSPDEV_VERSION=v20261001
# PPSSPP master provides ME instruction emulation (Core/MIPS/MeCore.cpp).
ARG PPSSPP_REF=44f1f44be7ff3af79d96e0d14e2d5f9e6ec923b7

# ---- toolchain + host packages ----
FROM ubuntu:${UBUNTU_VERSION} AS toolchain
ARG PSPDEV_VERSION
ENV DEBIAN_FRONTEND=noninteractive \
    PSPDEV=/opt/pspdev \
    PATH=/opt/pspdev/bin:$PATH
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake pkgconf ca-certificates curl git python3 bash \
      libreadline8 libusb-0.1-4 libgpgme11 libarchive-tools fakeroot \
      zlib1g-dev libssl-dev libcurl4-openssl-dev \
      ffmpeg libpng-dev libgl1-mesa-dev libglu1-mesa-dev libx11-dev \
      libsdl2-dev python3-pil \
 && rm -rf /var/lib/apt/lists/*
RUN curl -fL --retry 3 -o /tmp/pspdev.tgz \
      "https://github.com/pspdev/pspdev/releases/download/${PSPDEV_VERSION}/pspdev-ubuntu-latest-x86_64.tar.gz" \
 && tar -xzf /tmp/pspdev.tgz -C /opt \
 && rm /tmp/pspdev.tgz \
 && psp-gcc --version | head -1
# The workspace is mounted/checked out with a different owner than root.
RUN git config --system --add safe.directory '*'

# ---- prebuilt PSP FFmpeg + libcurl, built with the repo's own recipe ----
FROM toolchain AS deps
WORKDIR /src
COPY Makefile CMakeLists.txt ./
COPY cmake/ cmake/
COPY patches/ patches/
RUN make dependencies PSPDEV="$PSPDEV" PARALLEL="$(nproc)" \
 && mkdir -p /opt/wilimini-deps \
 && cp -a build/curl-install build/ffmpeg-audio-install build/ffmpeg-source build/ffmpeg-audio build/curl-source /opt/wilimini-deps/
# ffmpeg-source (about 100 MB) and ffmpeg-audio (its build dir, with the generated
# config.h) are kept: the client compiles against FFmpeg's internal headers
# (libavcodec/psp_profile.h comes from our patch) and packages its license. curl-source
# (30 MB) supplies libcurl's license and the patched select.c hashed into the build manifest.

# ---- PPSSPP headless (built in parallel with the stages above) ----
FROM toolchain AS ppsspp
ARG PPSSPP_REF
COPY tools/ppsspp/build-ppsspp-headless.sh /src/tools/ppsspp/
RUN git clone https://github.com/hrydgard/ppsspp.git /opt/ppsspp-src \
 && git -C /opt/ppsspp-src checkout --detach "$PPSSPP_REF" \
 && git -C /opt/ppsspp-src submodule update --init --recursive --depth 1 \
 && PPSSPP_SOURCE=/opt/ppsspp-src PPSSPP_BUILD_DIR=/opt/ppsspp-build \
    PPSSPP_HEADLESS_DIR=/opt/ppsspp-headless PPSSPP_USE_FFMPEG=OFF \
    JOBS="$(nproc)" sh /src/tools/ppsspp/build-ppsspp-headless.sh \
 && rm -rf /opt/ppsspp-src /opt/ppsspp-build

# ---- ME firmware: official 6.61 updater, verified by SHA-256 ----
FROM toolchain AS firmware
# prepare-me-firmware.py builds pspdecrypt with clang (preinstalled on GitHub runners).
# Installed here, not in `toolchain`, so the ci image's cached layers stay valid.
RUN apt-get update && apt-get install -y --no-install-recommends clang \
 && rm -rf /var/lib/apt/lists/*
COPY tools/ppsspp/prepare-me-firmware.py /src/tools/ppsspp/
RUN python3 /src/tools/ppsspp/prepare-me-firmware.py --output /opt/me-firmware/flash0

# ======== final targets ========

FROM toolchain AS ci
COPY --from=deps /opt/wilimini-deps /opt/wilimini-deps
ENV PSP_FFMPEG_PREFIX=/opt/wilimini-deps/ffmpeg-audio-install \
    PSP_CURL_PREFIX=/opt/wilimini-deps/curl-install \
    BILI_FFMPEG_SOURCE_DIR=/opt/wilimini-deps/ffmpeg-source \
    BILI_FFMPEG_BUILD_DIR=/opt/wilimini-deps/ffmpeg-audio \
    BILI_CURL_SOURCE_DIR=/opt/wilimini-deps/curl-source
WORKDIR /work

FROM ci AS ci-playback
COPY --from=ppsspp /opt/ppsspp-headless /opt/ppsspp-headless
COPY --from=firmware /opt/me-firmware /opt/me-firmware
ENV PPSSPP_HEADLESS=/opt/ppsspp-headless/PPSSPPHeadless \
    BILI_HEADLESS_FIRMWARE_DIR=/opt/me-firmware/flash0
