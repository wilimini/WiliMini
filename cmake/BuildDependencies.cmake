include(ExternalProject)

set(BILI_CURL_VERSION "8.10.1" CACHE STRING "libcurl version to build for PSP")
set(BILI_CURL_SHA256
    "73a4b0e99596a09fa5924a4fb7e4b995a85fda0d18a2c02ab9cf134bebce04ee"
    CACHE STRING "SHA-256 of the libcurl source archive")
set(BILI_CURL_SOURCE_DIR "${CMAKE_SOURCE_DIR}/build/curl-source" CACHE PATH
    "Extracted libcurl source directory")
set(BILI_CURL_BUILD_DIR "${CMAKE_SOURCE_DIR}/build/curl-build" CACHE PATH
    "libcurl build directory")
set(BILI_CURL_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/build/curl-install" CACHE PATH
    "libcurl install prefix")
set(BILI_FFMPEG_BUILD_DIR "${CMAKE_SOURCE_DIR}/build/ffmpeg-audio" CACHE PATH
    "FFmpeg build directory")

set(_curl_archive "${CMAKE_SOURCE_DIR}/build/downloads/curl-${BILI_CURL_VERSION}.tar.xz")
set(_curl_url "https://curl.se/download/curl-${BILI_CURL_VERSION}.tar.xz")

find_program(PSP_GCC psp-gcc REQUIRED)
find_program(PSP_AR psp-ar REQUIRED)
find_program(PSP_RANLIB psp-ranlib REQUIRED)

ExternalProject_Add(psp_curl
    URL "${_curl_url}"
    URL_HASH "SHA256=${BILI_CURL_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DOWNLOAD_DIR "${CMAKE_SOURCE_DIR}/build/downloads"
    SOURCE_DIR "${BILI_CURL_SOURCE_DIR}"
    BINARY_DIR "${BILI_CURL_BUILD_DIR}"
    PATCH_COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${BILI_CURL_SOURCE_DIR}"
        "-DPATCH_FILES=${CMAKE_SOURCE_DIR}/patches/curl-psp-select-pending.patch"
        -P "${CMAKE_SOURCE_DIR}/cmake/ApplyPatch.cmake"
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
        "CC=${PSP_GCC}" "AR=${PSP_AR}" "RANLIB=${PSP_RANLIB}"
        "CFLAGS=-O2 -G0"
        "CPPFLAGS=-I${PSPDEV}/psp/sdk/include"
        "LDFLAGS=-G0 -L${PSPDEV}/psp/sdk/lib -Wl,--defsym,module_info=0"
        "LIBS=-lpspnet_inet -lpspnet_apctl -lpspnet_resolver -lpspnet -lpsputility -lpsprtc -lpspsdk -lpspuser"
        <SOURCE_DIR>/configure
        --host=mipsel-unknown-elf
        --prefix=${BILI_CURL_INSTALL_PREFIX}
        --disable-shared --enable-static --disable-docs --disable-manual
        --disable-ldap --disable-ldaps --disable-ftp --disable-file --disable-rtsp
        --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap
        --disable-smtp --disable-gopher --disable-mqtt --disable-ipv6
        --disable-threaded-resolver --disable-unix-sockets
        --without-libidn2 --without-libpsl --without-brotli --without-zstd
        --with-mbedtls=${PSPDEV}/psp --with-zlib=${PSPDEV}/psp
        --without-ca-path --without-ca-bundle
    BUILD_COMMAND "${CMAKE_MAKE_PROGRAM}" -C <SOURCE_DIR>/lib --parallel 4
    INSTALL_COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${BILI_CURL_SOURCE_DIR}"
        "-DINSTALL_PREFIX=${BILI_CURL_INSTALL_PREFIX}"
        "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        -P "${CMAKE_SOURCE_DIR}/cmake/InstallCurl.cmake"
    BUILD_BYPRODUCTS "${BILI_CURL_INSTALL_PREFIX}/lib/libcurl.a")

if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/configure")
    ExternalProject_Add(psp_ffmpeg
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg"
        BINARY_DIR "${BILI_FFMPEG_BUILD_DIR}"
        PATCH_COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/third_party/ffmpeg"
            "-DPATCH_FILES=${CMAKE_SOURCE_DIR}/patches/ffmpeg/0001-psp-sampled-h264-profile.patch;${CMAKE_SOURCE_DIR}/patches/ffmpeg-psp-tx-tables.patch"
            -P "${CMAKE_SOURCE_DIR}/cmake/ApplyPatch.cmake"
        CONFIGURE_COMMAND "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/configure"
            --prefix=${PSP_FFMPEG_PREFIX}
            --enable-cross-compile --target-os=none --arch=mips --cpu=allegrex
            --cross-prefix=psp- --disable-asm --disable-autodetect
            --disable-programs --disable-doc --disable-debug --disable-network
            --disable-avdevice --disable-avfilter --disable-postproc
            --disable-swscale --disable-swresample --disable-pthreads
            --disable-everything --enable-decoder=h264,aac,aac_fixed --enable-parser=h264
            --enable-demuxer=mov,flv --enable-protocol=file
            "--extra-cflags=-O3 -G0 -Wno-error=incompatible-pointer-types"
            --extra-ldflags=-G0 --pkg-config=/usr/bin/false
        BUILD_COMMAND "${CMAKE_MAKE_PROGRAM}" --parallel 4
        INSTALL_COMMAND "${CMAKE_MAKE_PROGRAM}" install
        BUILD_BYPRODUCTS "${PSP_FFMPEG_PREFIX}/lib/libavcodec.a")
else()
    add_custom_target(psp_ffmpeg
        COMMAND "${CMAKE_COMMAND}" -E echo
            "third_party/ffmpeg is not initialized; run git submodule update --init --recursive"
        COMMAND "${CMAKE_COMMAND}" -E false)
endif()

add_custom_target(dependencies DEPENDS psp_curl psp_ffmpeg)
