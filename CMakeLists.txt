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

option(BILI_DEVICE_OSK_TEST "Open the PSP on-screen keyboard at startup for diagnostics" OFF)
option(BILI_DEVICE_PLAYBACK_TEST "Run controlled media decoder tests without account/network APIs" OFF)
option(BILI_DEVICE_PLAYBACK_CPU_RENDER "Diagnostic CPU display when ME firmware is unavailable" OFF)
option(BILI_PPSSPP_SCE_MPEG "Use the legacy PPSSPP sceMpeg backend instead of native ME" OFF)
option(BILI_MEMORY_PROFILE "Log large libc allocations for memory diagnostics" OFF)
option(BILI_PACKED_CHROMA "Use exact packed H.264 chroma kernels" ON)
option(BILI_BENCHMARK_ME_CACHE "Diagnostic whole-cache ME input writeback" OFF)
option(BILI_BENCHMARK_ME_CHECK "Diagnostic CPU/ME pixel comparison" OFF)
option(BILI_BENCHMARK_ME_FULL_CACHE "Diagnostic whole-cache maintenance on ME" OFF)
option(BILI_BENCHMARK_CABAC_CHECK "Diagnostic exact CABAC state oracle" OFF)
option(BILI_BENCHMARK_DSP_CHECK "Diagnostic integer qpel state oracle" OFF)
option(BILI_BENCHMARK_INTEGER_UNALIGNED "Diagnostic integer unaligned 16x16 copy" OFF)
option(BILI_BENCHMARK_UNROLLED_AVG "Diagnostic unrolled 16x16 rounded average" OFF)
set(BILI_BENCHMARK_PRESENT_QUEUE 0 CACHE STRING "Diagnostic ME presentation queue depth (0 or 2..16)")
if((BILI_BENCHMARK_ME_CACHE OR BILI_BENCHMARK_ME_CHECK OR BILI_BENCHMARK_ME_FULL_CACHE OR BILI_BENCHMARK_PRESENT_QUEUE
        OR BILI_BENCHMARK_CABAC_CHECK OR BILI_BENCHMARK_DSP_CHECK
        OR BILI_BENCHMARK_INTEGER_UNALIGNED OR BILI_BENCHMARK_UNROLLED_AVG)
        AND NOT BILI_DEVICE_PLAYBACK_TEST)
    message(FATAL_ERROR "Decoder experiments require BILI_DEVICE_PLAYBACK_TEST")
endif()
if(NOT BILI_BENCHMARK_PRESENT_QUEUE MATCHES "^(0|[2-9]|1[0-6])$")
    message(FATAL_ERROR "Presentation queue depth must be 0 or 2..16")
endif()

set(PSP_FFMPEG_PREFIX "${CMAKE_SOURCE_DIR}/build/ffmpeg-audio-install" CACHE PATH
    "PSP FFmpeg installation prefix")
set(PSP_CURL_PREFIX "${CMAKE_SOURCE_DIR}/build/curl-install" CACHE PATH
    "PSP libcurl installation prefix")
set(BILI_CA_BUNDLE "/etc/ssl/cert.pem" CACHE FILEPATH "CA bundle installed with the client")
set(BILI_DIST_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "Packaging root")
option(BILI_BUILD_CLIENT "Configure the PSP client and packaging targets" ON)
set(BILI_FONT_FILE "" CACHE FILEPATH "Font source used by the generate-font target")

set(ROOT "${CMAKE_SOURCE_DIR}")
set(PSPSDK "${PSPDEV}/psp/sdk")
set(ME_MIST_ROOT "${ROOT}/third_party/psp-media-engine-mist")
set(ME_CORE_ROOT "${ROOT}/third_party/psp-media-engine-custom-core")

execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${ROOT}"
    OUTPUT_VARIABLE BILI_BUILD_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE BILI_GIT_DESCRIBE_RESULT)
if(NOT BILI_GIT_DESCRIBE_RESULT EQUAL 0 OR NOT BILI_BUILD_VERSION)
    set(BILI_BUILD_VERSION "local")
endif()
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file("${ROOT}/cmake/build_info.hpp.in"
    "${CMAKE_BINARY_DIR}/generated/build_info.hpp" @ONLY)

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
    PSP_ME_RENDER_QUEUE=${BILI_BENCHMARK_PRESENT_QUEUE}
    PSP_HEADLESS_PROFILE=0
    PSP_VFPU_RENDER=0
    PSP_VFPU_DSP=4
    PSP_INTEGER_UNALIGNED_COPY=$<BOOL:${BILI_BENCHMARK_INTEGER_UNALIGNED}>
    PSP_UNROLLED_AVG=$<BOOL:${BILI_BENCHMARK_UNROLLED_AVG}>
    PSP_ME_RENDER=2
    PSP_ME_DMA=1
    PSP_ME_CHECK=$<BOOL:${BILI_BENCHMARK_ME_CHECK}>
    PSP_ME_CACHE=$<BOOL:${BILI_BENCHMARK_ME_CACHE}>
    PSP_ME_FULL_CACHE=$<BOOL:${BILI_BENCHMARK_ME_FULL_CACHE}>
    PSP_CABAC_SELFTEST=$<BOOL:${BILI_BENCHMARK_CABAC_CHECK}>
    PSP_ME_DECODE=0
    PSP_ME_DECODE_CHECK=0
    PSP_ME_DECODE_RENDER=0
    PSP_ME_DECODE_FAST=0
    PSP_ME_DECODE_SELECT=0
    PSP_ME_PIPE_BATCH=8
    PSP_ME_PIPE_FILTER=0
    PSP_SKIP_FRAME=-16
    PSP_SKIP_LOOP_FILTER=48
    PSP_APPROX_QPEL=2
    PSP_PACKED_CHROMA=$<BOOL:${BILI_PACKED_CHROMA}>
    PSP_DSP_SELFTEST=$<BOOL:${BILI_BENCHMARK_DSP_CHECK}>
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
target_compile_definitions(pspvideo PRIVATE BILI_PPSSPP_SCE_MPEG=$<BOOL:${BILI_PPSSPP_SCE_MPEG}>)
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
    decoder/me/mist_backend.c
    "${ME_EXPORTS_C}"
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

add_library(wili_client_objects OBJECT
    platform/src/psp_device.c
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
    client/localization.cpp
    "${CMAKE_BINARY_DIR}/generated/localization.generated.cpp"
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
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(LOCALIZATION_OUTPUTS
    "${CMAKE_BINARY_DIR}/generated/localization.generated.hpp"
    "${CMAKE_BINARY_DIR}/generated/localization.generated.cpp")
add_custom_command(
    OUTPUT ${LOCALIZATION_OUTPUTS}
    COMMAND "${Python3_EXECUTABLE}" "${ROOT}/scripts/generate-localization.py"
        --zh-hans "${ROOT}/resources/i18n/zh-Hans.json"
        --en-us "${ROOT}/resources/i18n/en-US.json"
        --output-dir "${CMAKE_BINARY_DIR}/generated"
    DEPENDS
        "${ROOT}/scripts/generate-localization.py"
        "${ROOT}/resources/i18n/zh-Hans.json"
        "${ROOT}/resources/i18n/en-US.json"
    VERBATIM
    COMMENT "Generating static localization tables")
add_custom_target(generate_localization DEPENDS ${LOCALIZATION_OUTPUTS})
add_dependencies(wili_client_objects generate_localization)
set(CLIENT_INCLUDE_DIRS
    "${CMAKE_BINARY_DIR}/generated"
    "${ROOT}/decoder/include"
    "${ROOT}/platform/include"
    "${ROOT}/third_party/nlohmann"
    "${ROOT}/third_party/pystring"
    "${ROOT}/client/api/wiliwili"
    "${ROOT}/client"
    "${PSP_CURL_PREFIX}/include")
target_include_directories(wili_client_objects PRIVATE ${CLIENT_INCLUDE_DIRS})
target_compile_definitions(wili_client_objects PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(wili_client_objects PRIVATE BILI_PPSSPP_SCE_MPEG=$<BOOL:${BILI_PPSSPP_SCE_MPEG}>)
if(BILI_MEMORY_PROFILE)
    target_sources(wili_client_objects PRIVATE client/memory_profile.c)
endif()

# Compile the application once; only startup heap/defaults differ between PRXs.
foreach(client_target wili_mini wili_mini_1000)
    add_executable(${client_target}
        client/main.cpp client/memory_budget.cpp
        $<TARGET_OBJECTS:wili_client_objects>)
    if(client_target STREQUAL "wili_mini_1000")
        target_compile_definitions(${client_target} PRIVATE BILI_CLIENT_HEAP_KB=12288 BILI_DEFAULT_RGB_FRAMES=2)
    else()
        target_compile_definitions(${client_target} PRIVATE BILI_CLIENT_HEAP_KB=24576 BILI_DEFAULT_RGB_FRAMES=8)
    endif()
    target_compile_definitions(${client_target} PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_definitions(${client_target} PRIVATE BILI_PPSSPP_SCE_MPEG=$<BOOL:${BILI_PPSSPP_SCE_MPEG}>)
    if(BILI_MEMORY_PROFILE)
        target_link_options(${client_target} PRIVATE -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=memalign)
    endif()
    if(BILI_DEVICE_PLAYBACK_TEST)
        target_compile_definitions(${client_target} PRIVATE BILI_DEVICE_PLAYBACK_TEST)
    endif()
    if(BILI_DEVICE_OSK_TEST)
        target_compile_definitions(${client_target} PRIVATE BILI_DEVICE_OSK_TEST)
    endif()
    target_include_directories(${client_target} PRIVATE ${CLIENT_INCLUDE_DIRS})

    target_link_options(${client_target} PRIVATE
        -G0
        "-Wl,-Map,${CMAKE_BINARY_DIR}/${client_target}.map"
        -Wl,--wrap=gethostbyname
        -Wl,--wrap=time
        -Wl,--wrap=gettimeofday
        -Wl,--wrap=ff_h264qpel_init
        -Wl,--wrap=ff_h264chroma_init
        -Wl,--wrap=avpriv_float_dsp_alloc
        "-specs=${PSPSDK}/lib/prxspecs"
        "-Wl,-q,-T${PSPSDK}/lib/linkfile.prx"
        -Wl,-zmax-page-size=128)
    target_link_libraries(${client_target} PRIVATE
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
        psputility psprtc pspkubridge
        pspdebug pspdisplay pspge pspctrl)
    if(BILI_PPSSPP_SCE_MPEG)
        target_link_libraries(${client_target} PRIVATE pspmpeg)
    endif()
    set_target_properties(${client_target} PROPERTIES
        OUTPUT_NAME ${client_target}
        SUFFIX ".elf"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_custom_command(TARGET ${client_target} POST_BUILD
        COMMAND "${PSPDEV}/bin/psp-fixup-imports" "$<TARGET_FILE:${client_target}>"
        COMMAND "${PSPDEV}/bin/psp-prxgen" "$<TARGET_FILE:${client_target}>" "${CMAKE_BINARY_DIR}/${client_target}.prx"
        COMMENT "Fixing imports and generating ${client_target}.prx")
endforeach()

add_executable(bootstrap client/bootstrap.c)
target_sources(bootstrap PRIVATE platform/src/psp_device.c)
target_compile_definitions(bootstrap PRIVATE BILI_PPSSPP_SCE_MPEG=$<BOOL:${BILI_PPSSPP_SCE_MPEG}>)
target_include_directories(bootstrap PRIVATE
    "${ROOT}/decoder/include"
    "${ROOT}/platform/include")
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
    pspdebug pspdisplay pspge pspctrl pspnet pspnet_apctl pspkubridge)
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
    COMMAND "${PSPDEV}/bin/mksfoex" -d MEMSIZE=1 "WiliMini" "${LOADER_PARAM_SFO}"
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
        "${CMAKE_BINARY_DIR}/wili_mini_1000.prx"
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
    DEPENDS wili_mini wili_mini_1000 me_bridge eboot
    COMMENT "Packaging PSP/GAME/WiliMini")

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
if(BILI_BUILD_CLIENT)
    add_custom_target(me DEPENDS me_bridge)
    add_dependencies(package assets)
endif()
