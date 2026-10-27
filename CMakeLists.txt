cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PSPDEV AND DEFINED ENV{PSPDEV})
    set(PSPDEV "$ENV{PSPDEV}")
endif()
if(NOT PSPDEV)
    message(FATAL_ERROR "PSPDEV is not set. Run with psp-cmake or set PSPDEV.")
endif()

project(wili_mini LANGUAGES C CXX ASM)

if(NOT PSP)
    message(FATAL_ERROR "Use psp-cmake or pass -DCMAKE_TOOLCHAIN_FILE=cmake/psp-toolchain.cmake")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

option(BILI_UI_REVIEW "Build offline native visual review screens" OFF)
option(BILI_DEVICE_OFFLINE_UI "Use local API fixtures and skip network startup for PPSSPP UI checks" OFF)
option(BILI_DEVICE_SMOKE "Run the automated client smoke flow" OFF)
option(BILI_DEVICE_SMOKE_SEARCH "Start the automated smoke flow from search" OFF)
option(BILI_DEVICE_SMOKE_HOTS "Start the automated smoke flow from hots" OFF)
option(BILI_DEVICE_SMOKE_FULL "Decode the full smoke video instead of a 300-frame sample" OFF)
option(BILI_DEVICE_OSK_TEST "Open the PSP on-screen keyboard at startup for diagnostics" OFF)
option(BILI_DEVICE_PLAYBACK_TEST "Run controlled media decoder tests without account/network APIs" OFF)
option(BILI_DEVICE_PLAYBACK_CPU_RENDER "Diagnostic CPU display when ME firmware is unavailable" OFF)
option(BILI_DEVICE_SMOKE_NEXT_PAGE "Load the second list page in the automated smoke flow" OFF)
set(BILI_DEVICE_SMOKE_INDEX "0" CACHE STRING "Card index selected by the automated smoke flow")

if(BILI_DEVICE_SMOKE_SEARCH OR BILI_DEVICE_SMOKE_HOTS OR BILI_DEVICE_SMOKE_FULL)
    set(BILI_DEVICE_SMOKE ON)
endif()

set(PSP_FFMPEG_PREFIX "${CMAKE_SOURCE_DIR}/build/ffmpeg-audio-install" CACHE PATH
    "PSP FFmpeg installation prefix")
set(PSP_CURL_PREFIX "${CMAKE_SOURCE_DIR}/build/curl-install" CACHE PATH
    "PSP libcurl installation prefix")
set(BILI_CA_BUNDLE "/etc/ssl/cert.pem" CACHE FILEPATH "CA bundle installed with the client")
set(BILI_DIST_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "Packaging root")
option(BILI_BUILD_CLIENT "Configure the PSP client and packaging targets" ON)
set(BILI_FONT_FILE "" CACHE FILEPATH "Font source used by the generate-font target")
set(BILI_WILIWILI_DIR "" CACHE PATH "Wiliwili checkout used by the UI asset generator")

set(ROOT "${CMAKE_SOURCE_DIR}")
set(PSPSDK "${PSPDEV}/psp/sdk")
set(ME_MIST_ROOT "${ROOT}/third_party/psp-media-engine-mist")
set(ME_CORE_ROOT "${ROOT}/third_party/psp-media-engine-custom-core")

include("${ROOT}/cmake/BuildDependencies.cmake")

if(BILI_BUILD_CLIENT)
foreach(required
        "${PSP_FFMPEG_PREFIX}/lib/libavformat.a"
        "${PSP_FFMPEG_PREFIX}/lib/libavcodec.a"
        "${PSP_FFMPEG_PREFIX}/lib/libavutil.a"
        "${PSP_CURL_PREFIX}/lib/libcurl.a"
        "${ME_MIST_ROOT}/me-mist.c"
        "${ME_CORE_ROOT}/me-core-mapping.c")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "Missing dependency: ${required}")
    endif()
endforeach()

add_compile_options(-O2 -G0 -Wall -Wextra)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fexceptions>)

set(ME_INCLUDE_ROOT "${CMAKE_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${ME_INCLUDE_ROOT}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/me")
file(REMOVE_RECURSE "${ME_INCLUDE_ROOT}/me-core-mapper")
file(CREATE_LINK "${ME_CORE_ROOT}" "${ME_INCLUDE_ROOT}/me-core-mapper" SYMBOLIC)

set(DECODER_DEFINITIONS
    _POSIX_C_SOURCE=200809L
    PSP_BUILD_ID=\"wili-mini-demo-01\"
    PSP_HEADLESS_BENCH=0
    PSP_BENCH_FULL=0
    PSP_PTS_PACE=1
    PSP_AUDIO=1
    PSP_AUDIO_FIXED=0
    PSP_AUDIO_VFPU=1
    PSP_AUDIO_ADAPTIVE=1
    PSP_AUDIO_VIDEO_LEAD_US=800000
    PSP_AUDIO_PRESENT_IMMEDIATE=1
    PSP_RGB_PACE=1
    PSP_AUDIO_VERIFY=0
    PSP_RGB_NEAREST_PTS=1
    PSP_ME_RENDER_QUEUE=0
    PSP_HEADLESS_PROFILE=0
    PSP_VFPU_RENDER=0
    PSP_VFPU_DSP=4
    PSP_ME_RENDER=2
    PSP_ME_DMA=1
    PSP_ME_CHECK=0
    PSP_ME_CACHE=0
    PSP_ME_DECODE=0
    PSP_ME_DECODE_CHECK=0
    PSP_ME_DECODE_RENDER=0
    PSP_ME_DECODE_FAST=0
    PSP_ME_DECODE_SELECT=0
    PSP_ME_PIPE_BATCH=8
    PSP_ME_PIPE_FILTER=0
    PSP_SKIP_FRAME=8
    PSP_SKIP_LOOP_FILTER=48
    PSP_APPROX_QPEL=1
    PSP_PACKED_CHROMA=0
    PSP_DSP_SELFTEST=0
    PSP_YUV_LUT=1
    PSP_SPATIAL_LUT=1
    PSP_GROUPED_RENDER=1
    PSP_CLIP_LUT=1
    PSP_PAIRED_ROWS=1
    PSP_HALF_RENDER=0
    PSP_HEADLESS_VERIFY=0)

if(BILI_DEVICE_PLAYBACK_CPU_RENDER)
    if(NOT BILI_DEVICE_PLAYBACK_TEST)
        message(FATAL_ERROR "CPU diagnostic display requires BILI_DEVICE_PLAYBACK_TEST")
    endif()
    list(REMOVE_ITEM DECODER_DEFINITIONS PSP_ME_RENDER=2 PSP_RGB_PACE=1)
    list(APPEND DECODER_DEFINITIONS PSP_ME_RENDER=0 PSP_RGB_PACE=0)
endif()

add_library(pspvideo STATIC
    decoder/src/player.c
    decoder/src/vfpu_dsp.c
    decoder/src/audio_vfpu.c
    decoder/me/me_entry.S
    decoder/me/me_lab.S
    "${ME_CORE_ROOT}/me-core-mapping.c")
target_compile_definitions(pspvideo PRIVATE ${DECODER_DEFINITIONS})
target_include_directories(pspvideo PRIVATE
    "${ROOT}/decoder/include"
    "${ROOT}/decoder/src"
    "${ROOT}/decoder/me"
    "${BILI_FFMPEG_BUILD_DIR}"
    "${BILI_FFMPEG_SOURCE_DIR}"
    "${BILI_FFMPEG_SOURCE_DIR}/libavcodec"
    "${PSP_FFMPEG_PREFIX}/include"
    "${ME_INCLUDE_ROOT}"
    "${ME_CORE_ROOT}")

set(ME_EXPORTS_C "${CMAKE_BINARY_DIR}/me/exports.c")
add_custom_command(
    OUTPUT "${ME_EXPORTS_C}"
    COMMAND "${CMAKE_COMMAND}"
        "-DPSP_BUILD_EXPORTS=${PSPDEV}/bin/psp-build-exports"
        "-DEXPORTS_EXP=${ROOT}/decoder/me/exports.exp"
        "-DOUTPUT_FILE=${ME_EXPORTS_C}"
        -P "${ROOT}/cmake/BuildExports.cmake"
    DEPENDS "${ROOT}/decoder/me/exports.exp" "${ROOT}/cmake/BuildExports.cmake"
    COMMENT "Generating ME bridge exports")

add_executable(me_bridge
    decoder/me/bridge.c
    "${ME_EXPORTS_C}"
    "${ME_MIST_ROOT}/me-mist.c"
    "${ME_MIST_ROOT}/me-mist-utils.c"
    "${ME_CORE_ROOT}/me-core-mapping.c")
target_compile_options(me_bridge PRIVATE -ffunction-sections -fdata-sections)
target_include_directories(me_bridge PRIVATE
    "${ROOT}/decoder/me"
    "${ME_MIST_ROOT}"
    "${ME_CORE_ROOT}"
    "${ME_INCLUDE_ROOT}")
target_link_options(me_bridge PRIVATE
    -G0
    -nostartfiles
    "-Wl,-q,-T${PSPSDK}/lib/linkfile.prx"
    -Wl,-zmax-page-size=128)
target_link_libraries(me_bridge PRIVATE pspkernel pspsdk)
set_target_properties(me_bridge PROPERTIES
    OUTPUT_NAME me_bridge
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_custom_command(TARGET me_bridge POST_BUILD
    COMMAND "${PSPDEV}/bin/psp-fixup-imports" "$<TARGET_FILE:me_bridge>"
    COMMAND "${PSPDEV}/bin/psp-prxgen" "$<TARGET_FILE:me_bridge>" "${CMAKE_BINARY_DIR}/me_bridge.prx"
    COMMENT "Fixing imports and generating me_bridge.prx")

add_executable(wili_mini
    client/main.cpp
    client/app.cpp
    client/preferences.cpp
    client/app_settings.cpp
    client/playback.cpp
    client/api_client.cpp
    client/services/app_error.cpp
    client/services/playback_error.cpp
    client/services/network.cpp
    client/services/http_policy.c
    client/services/session_store.cpp
    client/services/auth_service.cpp
    client/services/search_service.cpp
    client/services/community_service.cpp
    client/app_community.cpp
    client/third_party/qrcodegen/qrcodegen.c
    client/ui.cpp
    client/bitmap_font.cpp
    client/bilibili.c
    client/live_stream.c
    client/json.c
    client/psp_clock.c
    client/psp_select.c
    client/psp_dns.c
    client/dns_packet.c)
target_compile_definitions(wili_mini PRIVATE _POSIX_C_SOURCE=200809L)
if(BILI_DEVICE_PLAYBACK_TEST)
    target_compile_definitions(wili_mini PRIVATE BILI_DEVICE_PLAYBACK_TEST)
endif()
if(BILI_UI_REVIEW)
    target_compile_definitions(wili_mini PRIVATE BILI_UI_REVIEW)
endif()
if(BILI_DEVICE_OFFLINE_UI)
    target_compile_definitions(wili_mini PRIVATE BILI_DEVICE_OFFLINE_UI)
endif()
if(BILI_DEVICE_SMOKE)
    target_compile_definitions(wili_mini PRIVATE BILI_DEVICE_SMOKE)
endif()
if(BILI_DEVICE_SMOKE_SEARCH)
    target_compile_definitions(wili_mini PRIVATE BILI_DEVICE_SMOKE_SEARCH)
endif()
if(BILI_DEVICE_SMOKE_HOTS)
    target_compile_definitions(wili_mini PRIVATE BILI_DEVICE_SMOKE_HOTS)
endif()
if(BILI_DEVICE_SMOKE_FULL)
    target_compile_definitions(wili_mini PRIVATE BILI_DEVICE_SMOKE_FULL)
endif()
if(BILI_DEVICE_OSK_TEST)
    target_compile_definitions(wili_mini PRIVATE BILI_DEVICE_OSK_TEST)
endif()
if(BILI_DEVICE_SMOKE_NEXT_PAGE)
    target_compile_definitions(wili_mini PRIVATE BILI_DEVICE_SMOKE_NEXT_PAGE)
endif()
if(NOT BILI_DEVICE_SMOKE_INDEX STREQUAL "0")
    target_compile_definitions(wili_mini PRIVATE BILI_DEVICE_SMOKE_INDEX=${BILI_DEVICE_SMOKE_INDEX})
endif()
target_include_directories(wili_mini PRIVATE
    "${ROOT}/decoder/include"
    "${ROOT}/third_party/nlohmann"
    "${ROOT}/third_party/pystring"
    "${ROOT}/client/api/wiliwili"
    "${ROOT}/client"
    "${PSP_CURL_PREFIX}/include")
target_link_options(wili_mini PRIVATE
    -G0
    "-Wl,-Map,${CMAKE_BINARY_DIR}/wili_mini.map"
    -Wl,--wrap=gethostbyname
    -Wl,--wrap=time
    -Wl,--wrap=gettimeofday
    -Wl,--wrap=ff_h264qpel_init
    -Wl,--wrap=ff_h264chroma_init
    -Wl,--wrap=avpriv_float_dsp_alloc
    "-specs=${PSPSDK}/lib/prxspecs"
    "-Wl,-q,-T${PSPSDK}/lib/linkfile.prx"
    -Wl,-zmax-page-size=128)
target_link_libraries(wili_mini PRIVATE
    pspvideo
    "${PSP_FFMPEG_PREFIX}/lib/libavformat.a"
    "${PSP_FFMPEG_PREFIX}/lib/libavcodec.a"
    "${PSP_FFMPEG_PREFIX}/lib/libavutil.a"
    "${PSP_CURL_PREFIX}/lib/libcurl.a"
    mbedtls mbedx509 mbedcrypto jpeg
    pspgu pspdebug m z
    -Wl,--start-group
    stdc++ supc++ pthreadglue pthread cglue c
    -Wl,--end-group
    pspge pspaudio pspdmac psppower
    pspnet_apctl pspnet_resolver pspnet_inet pspnet
    psputility psprtc
    pspdebug pspdisplay pspge pspctrl)
set_target_properties(wili_mini PROPERTIES
    OUTPUT_NAME wili_mini
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_custom_command(TARGET wili_mini POST_BUILD
    COMMAND "${PSPDEV}/bin/psp-fixup-imports" "$<TARGET_FILE:wili_mini>"
    COMMAND "${PSPDEV}/bin/psp-prxgen" "$<TARGET_FILE:wili_mini>" "${CMAKE_BINARY_DIR}/wili_mini.prx"
    COMMENT "Fixing imports and generating wili_mini.prx")

add_executable(bootstrap client/bootstrap.c)
if(BILI_UI_REVIEW)
    target_compile_definitions(bootstrap PRIVATE BILI_UI_REVIEW)
endif()
target_link_options(bootstrap PRIVATE
    -G0
    "-specs=${PSPSDK}/lib/prxspecs"
    "-Wl,-q,-T${PSPSDK}/lib/linkfile.prx"
    -Wl,-zmax-page-size=128)
target_link_libraries(bootstrap PRIVATE
    pspdebug
    -Wl,--start-group
    pthreadglue pthread cglue c
    -Wl,--end-group
    psputility
    pspdebug pspdisplay pspge pspctrl pspnet pspnet_apctl)
set_target_properties(bootstrap PROPERTIES
    OUTPUT_NAME bootstrap
    SUFFIX ".elf"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/loader")
add_custom_command(TARGET bootstrap POST_BUILD
    COMMAND "${PSPDEV}/bin/psp-fixup-imports" "$<TARGET_FILE:bootstrap>"
    COMMAND "${PSPDEV}/bin/psp-prxgen" "$<TARGET_FILE:bootstrap>" "${CMAKE_BINARY_DIR}/loader/bootstrap.prx"
    COMMENT "Generating loader bootstrap.prx")

set(LOADER_PARAM_SFO "${CMAKE_BINARY_DIR}/loader/PARAM.SFO")
set(LOADER_EBOOT_PBP "${CMAKE_BINARY_DIR}/loader/EBOOT.PBP")
set(ICON0_PNG "${ROOT}/assets/ICON0.png")
add_custom_command(OUTPUT "${LOADER_EBOOT_PBP}"
    BYPRODUCTS "${LOADER_PARAM_SFO}"
    COMMAND "${PSPDEV}/bin/mksfoex" -d MEMSIZE=2 "WiliMini" "${LOADER_PARAM_SFO}"
    COMMAND "${PSPDEV}/bin/pack-pbp"
        "${LOADER_EBOOT_PBP}"
        "${LOADER_PARAM_SFO}"
        "${ICON0_PNG}"
        NULL NULL NULL NULL
        "${CMAKE_BINARY_DIR}/loader/bootstrap.prx"
        NULL
    DEPENDS bootstrap "${ICON0_PNG}" "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Generating loader EBOOT.PBP")
add_custom_target(eboot DEPENDS "${LOADER_EBOOT_PBP}")

set(PACKAGE_DIR "${BILI_DIST_DIR}/PSP/GAME/WiliMini")
add_custom_target(package
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_DIR}/licenses"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PACKAGE_DIR}/cache"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/LICENSE" "${PACKAGE_DIR}/licenses/wili-mini.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_BINARY_DIR}/loader/EBOOT.PBP"
        "${CMAKE_BINARY_DIR}/wili_mini.prx"
        "${CMAKE_BINARY_DIR}/me_bridge.prx"
        "${PACKAGE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BILI_CA_BUNDLE}" "${PACKAGE_DIR}/cacert.pem"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/client/assets/font/wqy.bmf" "${PACKAGE_DIR}/wqy.bmf"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/client/assets/font/LICENSE.txt" "${PACKAGE_DIR}/licenses/wqy.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/client/assets/font/APACHE-2.0.txt" "${PACKAGE_DIR}/licenses/wqy-apache-2.0.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/client/assets/wiliwili/WILIWILI-LICENSE.txt" "${PACKAGE_DIR}/licenses/wiliwili-assets.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/build/curl-source/COPYING" "${PACKAGE_DIR}/licenses/curl.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BILI_FFMPEG_SOURCE_DIR}/COPYING.LGPLv2.1" "${PACKAGE_DIR}/licenses/ffmpeg.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/decoder/me/LICENSE.MIST.txt" "${PACKAGE_DIR}/licenses/me-mist.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/client/api/wiliwili/LICENSE" "${PACKAGE_DIR}/licenses/wiliwili.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/client/third_party/qrcodegen/LICENSE.txt" "${PACKAGE_DIR}/licenses/qrcodegen.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/client/third_party/pspsdk/LICENSE" "${PACKAGE_DIR}/licenses/pspsdk.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${ROOT}/third_party/nlohmann/LICENSE.MIT" "${PACKAGE_DIR}/licenses/nlohmann-json.txt"
    COMMAND python3 "${ROOT}/scripts/write-client-manifest.py"
        --root "${ROOT}"
        --package "${PACKAGE_DIR}"
        --ffmpeg "${PSP_FFMPEG_PREFIX}"
        --ffmpeg-source "${BILI_FFMPEG_SOURCE_DIR}"
        --curl "${PSP_CURL_PREFIX}"
    DEPENDS wili_mini me_bridge eboot
    COMMENT "Packaging PSP/GAME/WiliMini")

if(BILI_UI_REVIEW)
    add_custom_command(TARGET package POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ROOT}/client/assets/wiliwili/FONT-LICENSE.txt"
            "${ROOT}/client/assets/wiliwili/WILIWILI-LICENSE.txt"
            "${PACKAGE_DIR}/licenses")
endif()
endif()

add_custom_target(assets)
if(BILI_FONT_FILE)
    add_custom_target(generate-font
        COMMAND python3 "${ROOT}/scripts/prepare-client-font.py" "${BILI_FONT_FILE}"
        DEPENDS "${BILI_FONT_FILE}"
        BYPRODUCTS "${ROOT}/client/assets/font/wqy.bmf"
        COMMENT "Generating PSP bitmap font")
    add_dependencies(assets generate-font)
endif()
if(BILI_WILIWILI_DIR)
    add_custom_target(generate-ui-assets
        COMMAND python3 "${ROOT}/scripts/prepare-ui-review-assets.py" "${BILI_WILIWILI_DIR}"
        BYPRODUCTS "${ROOT}/client/assets/wiliwili/generated.hpp"
        COMMENT "Generating UI review assets")
    add_dependencies(assets generate-ui-assets)
endif()

if(BILI_BUILD_CLIENT)
    add_custom_target(me DEPENDS me_bridge)
    add_dependencies(package assets)
endif()
