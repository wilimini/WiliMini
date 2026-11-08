# CI images

GitHub Actions builds and tests inside images defined by `docker/ci.Dockerfile`. The
Dockerfile only caches dependencies; it never builds or tests WiliMini. Workflows check
out the repository and build the package themselves.

| Image | Target | Contents | Visibility |
| --- | --- | --- | --- |
| `ghcr.io/<owner>/wilimini-ci` | `ci` | Ubuntu, PSPDEV toolchain, host packages, prebuilt PSP FFmpeg and libcurl | public is fine |
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

The image provides `PSP_FFMPEG_PREFIX` and `PSP_CURL_PREFIX`. Use CMake directly; `make
package` also rebuilds FFmpeg and libcurl:

```sh
docker build -f docker/ci.Dockerfile --target ci -t wilimini-ci .
docker run --rm -v "$PWD":/work -w /work wilimini-ci sh -c '
  psp-cmake -S . -B build/client -DCMAKE_BUILD_TYPE=Release \
    -DPSP_FFMPEG_PREFIX="$PSP_FFMPEG_PREFIX" -DPSP_CURL_PREFIX="$PSP_CURL_PREFIX" \
    -DBILI_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt &&
  cmake --build build/client --target package --parallel "$(nproc)"'
```

The image build does not need the PSP toolchain on the host. Only the checks that are
unaffected by the image (`tools/test-host.sh`) still run on the plain runner.
