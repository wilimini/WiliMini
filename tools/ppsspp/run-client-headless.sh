#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
runner=${PPSSPP_HEADLESS:-"$root/build/ppsspp-headless/PPSSPPHeadless"}
memstick=${BILI_HEADLESS_MEMSTICK:-"$root/build/headless/memstick"}
client_dir=${BILI_HEADLESS_CLIENT_DIR:-"$root/build/client"}
package=${BILI_HEADLESS_PACKAGE:-"$root/dist/PSP/GAME/WiliMini"}
font_dir="$root/build/ppsspp-headless/assets/flash0/font"
output=${BILI_HEADLESS_SCREENSHOT:-"$root/results/headless/home.png"}
screenshot_at=${BILI_HEADLESS_SCREENSHOT_AT:-"5.0:$output"}
timeout=${BILI_HEADLESS_TIMEOUT:-7}
timeout_emulated=${BILI_HEADLESS_TIMEOUT_EMULATED:-}
model=${BILI_HEADLESS_MODEL:-1000}
case "$model" in
    1000) model_id=0 ;;
    2000|3000) model_id=1 ;;
    *) echo "BILI_HEADLESS_MODEL must be 1000, 2000 or 3000." >&2; exit 1 ;;
esac

test -x "$runner" || { echo "Build PPSSPPHeadless first (tools/ppsspp/build-ppsspp-headless.sh)." >&2; exit 1; }
test -f "$client_dir/loader/bootstrap.prx" || { echo "Build the client first (make package)." >&2; exit 1; }
test -f "$package/wili_mini.prx" || { echo "Missing packaged client: $package" >&2; exit 1; }
test -f "$package/wili_mini_1000.prx" || { echo "Missing PSP-1000 client: $package" >&2; exit 1; }

mkdir -p "$memstick/PSP/GAME" "$memstick/PSP/NAND/flash0/font" "$(dirname -- "$output")"
printf '[SystemParam]\nPSPModel = %s\n' "$model_id" > "$memstick/headless-model.ini"
if [ ! -d "$memstick/PSP/GAME/WiliMini" ]; then
    cp -R "$package" "$memstick/PSP/GAME/WiliMini"
fi
for artifact in EBOOT.PBP wili_mini.prx wili_mini_1000.prx me_bridge.prx cacert.pem build.json wqy.bmf; do
    cp -f "$package/$artifact" "$memstick/PSP/GAME/WiliMini/$artifact"
done
if [ ! -f "$memstick/PSP/GAME/WiliMini/wifi-profile.txt" ]; then
    printf '1\n' > "$memstick/PSP/GAME/WiliMini/wifi-profile.txt"
fi

# PPSSPP Headless does not mount flash0 fonts unless a NAND font directory exists.
if [ -f "$font_dir/ltn0.pgf" ]; then
    cp -f "$font_dir/ltn0.pgf" "$memstick/PSP/NAND/flash0/font/ltn0.pgf"
fi
if [ -f "$font_dir/jpn0.pgf" ]; then
    cp -f "$font_dir/jpn0.pgf" "$memstick/PSP/NAND/flash0/font/gb3s1518.bwfon"
fi

set -- \
    "--appendconfig=$memstick/headless-model.ini" \
    --graphics=software \
    --me-core \
    "--root=$root" \
    "--memstick=$memstick" \
    "--screenshot-at=$screenshot_at" \
    "--timeout=$timeout"
if [ -n "$timeout_emulated" ]; then
    set -- "$@" "--timeout-emulated=$timeout_emulated"
fi
if [ -n "${BILI_HEADLESS_PRESS_SCRIPT:-}" ]; then
    set -- "$@" "--press=$BILI_HEADLESS_PRESS_SCRIPT"
fi
"$runner" "$@" "$client_dir/loader/bootstrap.prx"
