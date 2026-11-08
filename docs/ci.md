# CI images

GitHub Actions builds and tests inside images defined by `docker/ci.Dockerfile`. The
Dockerfile only caches dependencies; it never builds or tests WiliMini. Workflows check
out the repository and build the package themselves.

| Image | Target | Contents | Visibility |
| --- | --- | --- | --- |
| `ghcr.io/<owner>/wilimini-ci` | `ci` | Ubuntu, PSPDEV toolchain, host packages, prebuilt PSP FFmpeg and libcurl, patched FFmpeg source | public is fine |
| `ghcr.io/<owner>/wilimini-ci-playback` | `ci-playback` | `ci` + PPSSPP headless + ME firmware | **private only** |

`ci-playback` contains the firmware images extracted from the official PSP 6.61 updater.
Keep that package private. `.github/workflows/ci-image.yml` fails if it can be pulled
anonymously.

## How images are selected

`ci-image.yml` is called by `build.yml` and `test.yml`. It tags both images with a hash of
`docker/ci.Dockerfile`, `.dockerignore`, `Makefile`, `CMakeLists.txt`, `cmake/`, `patches/`
and the two `tools/ppsspp` scripts the image uses, and builds only the images whose tag is
missing. Layers are cached in the GitHub Actions cache. Pull requests from forks cannot
push images: if their change needs a new image, CI stops with an error and a maintainer
must run the CI image workflow on the base repository.

Pins (`PSPDEV_VERSION`, `PPSSPP_REF`) live only in the Dockerfile. Changing the PSP
dependency recipe, its patches or a pin produces a new tag and rebuilds the affected layers.

## Building packages in the image

Packaging reads `build/curl-source` (libcurl's license and the patched `select.c` hashed into the
build manifest) from a fixed path, so jobs first run
`mkdir -p build && ln -sfn "$BILI_CURL_SOURCE_DIR" build/curl-source`.

The image provides `PSP_FFMPEG_PREFIX`, `PSP_CURL_PREFIX` and `BILI_FFMPEG_SOURCE_DIR` and
`BILI_FFMPEG_BUILD_DIR` (the patched FFmpeg source and its build directory with the generated
`config.h`: the client compiles against FFmpeg's internal headers and packages its license). Use CMake directly; `make
package` also rebuilds FFmpeg and libcurl:

```sh
docker build -f docker/ci.Dockerfile --target ci -t wilimini-ci .
docker run --rm -v "$PWD":/work -w /work wilimini-ci sh -c '
  mkdir -p build && ln -sfn "$BILI_CURL_SOURCE_DIR" build/curl-source &&
  psp-cmake -S . -B build/client -DCMAKE_BUILD_TYPE=Release \
    -DPSP_FFMPEG_PREFIX="$PSP_FFMPEG_PREFIX" -DPSP_CURL_PREFIX="$PSP_CURL_PREFIX" \
    -DBILI_FFMPEG_SOURCE_DIR="$BILI_FFMPEG_SOURCE_DIR" -DBILI_FFMPEG_BUILD_DIR="$BILI_FFMPEG_BUILD_DIR" \
    -DBILI_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt &&
  cmake --build build/client --target package --parallel "$(nproc)"'
```

The image build does not need the PSP toolchain on the host. Only the checks that are
unaffected by the image (`tools/test-host.sh`) still run on the plain runner.
