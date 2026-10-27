#!/bin/sh
# Build against this PSPSDK's BSD socket headers and installed mbedTLS.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
export PSPDEV=${PSPDEV:-/Users/liu/pspdev}
export PATH="$PSPDEV/bin:$PATH"
archive="$root/build/downloads/curl-8.10.1.tar.xz"
mkdir -p "$root/build/downloads" "$root/build/curl-source"
python3 - "$archive" <<'PY'
import hashlib,sys,urllib.request
from pathlib import Path
p=Path(sys.argv[1])
if not p.exists():
    p.write_bytes(urllib.request.urlopen('https://curl.se/download/curl-8.10.1.tar.xz',timeout=60).read())
expected='73a4b0e99596a09fa5924a4fb7e4b995a85fda0d18a2c02ab9cf134bebce04ee'
if hashlib.sha256(p.read_bytes()).hexdigest()!=expected:raise SystemExit('curl source SHA-256 mismatch')
PY
if [ ! -f "$root/build/curl-source/configure" ]; then
    tar -xf "$archive" -C "$root/build/curl-source" --strip-components=1
fi
# Preserve the PSP asynchronous-connect compatibility fix across extraction.
select_patch="$root/patches/curl-psp-select-pending.patch"
if ! patch -d "$root/build/curl-source" -p1 -R --dry-run < "$select_patch" >/dev/null 2>&1; then
    patch -d "$root/build/curl-source" -p1 --forward < "$select_patch"
fi
cd "$root/build/curl-source"
./configure --host=mipsel-unknown-elf --prefix="$root/build/curl-install" \
    --disable-shared --enable-static --disable-docs --disable-manual \
    --disable-ldap --disable-ldaps --disable-ftp --disable-file --disable-rtsp \
    --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
    --disable-smtp --disable-gopher --disable-mqtt --disable-ipv6 \
    --disable-threaded-resolver --disable-unix-sockets \
    --without-libidn2 --without-libpsl --without-brotli --without-zstd \
    --with-mbedtls="$PSPDEV/psp" --with-zlib="$PSPDEV/psp" \
    --without-ca-path --without-ca-bundle \
    CC=psp-gcc AR=psp-ar RANLIB=psp-ranlib CFLAGS='-O2 -G0' \
    CPPFLAGS="-I$PSPDEV/psp/sdk/include" \
    LDFLAGS="-G0 -L$PSPDEV/psp/sdk/lib -Wl,--defsym,module_info=0" \
    LIBS='-lpspnet_inet -lpspnet_apctl -lpspnet_resolver -lpspnet -lpsputility -lpsprtc -lpspsdk -lpspuser'
make -C lib -j4
make -C lib install
make -C include install
cp COPYING "$root/build/curl-install/COPYING"
