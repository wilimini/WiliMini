#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PSPDEV=${PSPDEV:-/Users/liu/pspdev}
export PATH="$PSPDEV/bin:$PATH"

out=${BILI_BUILD_DIR:-"$root/build/client"}
ffmpeg=${PSP_FFMPEG_PREFIX:-"$root/build/ffmpeg-audio-install"}
curl_prefix=${PSP_CURL_PREFIX:-"$root/build/curl-install"}

if [ ! -f "$curl_prefix/lib/libcurl.a" ]; then
    if [ "$curl_prefix" != "$root/build/curl-install" ]; then
        echo "Missing PSP_CURL_PREFIX/lib/libcurl.a" >&2
        exit 1
    fi
    sh "$root/scripts/build-client-curl.sh"
fi
if [ ! -f "$ffmpeg/lib/libavcodec.a" ]; then
    echo "Missing FFmpeg libraries in $ffmpeg" >&2
    exit 1
fi

smoke=OFF
smoke_search=OFF
smoke_hots=OFF
smoke_full=OFF
osk_test=OFF
smoke_next_page=OFF
offline_ui=OFF
smoke_index=${BILI_DEVICE_SMOKE_INDEX:-0}
case " ${CLIENT_FLAGS:-} " in
    *" -DBILI_DEVICE_SMOKE_SEARCH "*) smoke_search=ON ;;
esac
case " ${CLIENT_FLAGS:-} " in
    *" -DBILI_DEVICE_SMOKE_HOTS "*) smoke_hots=ON ;;
esac
case " ${CLIENT_FLAGS:-} " in
    *" -DBILI_DEVICE_SMOKE_FULL "*) smoke_full=ON ;;
esac
case " ${CLIENT_FLAGS:-} " in
    *" -DBILI_DEVICE_SMOKE "*) smoke=ON ;;
esac
case " ${CLIENT_FLAGS:-} " in
    *" -DBILI_DEVICE_OSK_TEST "*) osk_test=ON ;;
esac
case " ${CLIENT_FLAGS:-} " in
    *" -DBILI_DEVICE_SMOKE_NEXT_PAGE "*) smoke_next_page=ON ;;
esac
case " ${CLIENT_FLAGS:-} " in
    *" -DBILI_DEVICE_OFFLINE_UI "*) offline_ui=ON ;;
esac

"$PSPDEV/bin/psp-cmake" \
    -S "$root" \
    -B "$out" \
    -DCMAKE_BUILD_TYPE="${BILI_BUILD_TYPE:-}" \
    -DPSP_FFMPEG_PREFIX="$ffmpeg" \
    -DPSP_CURL_PREFIX="$curl_prefix" \
    -DBILI_CA_BUNDLE="${PSP_CA_BUNDLE:-/etc/ssl/cert.pem}" \
    -DBILI_DEVICE_SMOKE="$smoke" \
    -DBILI_DEVICE_SMOKE_SEARCH="$smoke_search" \
    -DBILI_DEVICE_SMOKE_HOTS="$smoke_hots" \
    -DBILI_DEVICE_SMOKE_FULL="$smoke_full" \
    -DBILI_DEVICE_OSK_TEST="$osk_test" \
    -DBILI_DEVICE_SMOKE_NEXT_PAGE="$smoke_next_page" \
    -DBILI_DEVICE_OFFLINE_UI="$offline_ui" \
    -DBILI_DEVICE_SMOKE_INDEX="$smoke_index" \
    "$@"

cmake --build "$out" --target package --parallel 4
