#!/usr/bin/env python3
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


parser = argparse.ArgumentParser()
parser.add_argument("--root", required=True, type=Path)
parser.add_argument("--package", required=True, type=Path)
parser.add_argument("--ffmpeg", required=True, type=Path)
parser.add_argument("--ffmpeg-source", type=Path)
parser.add_argument("--curl", required=True, type=Path)
args = parser.parse_args()

root = args.root.resolve()
package = args.package.resolve()
ffmpeg = args.ffmpeg.resolve()
ffmpeg_source = (args.ffmpeg_source or root / "build/ffmpeg-source").resolve()
curl = args.curl.resolve()

sources = [
    *root.glob("client/*.[ch]"),
    *root.glob("client/*.hpp"),
    *root.glob("client/*.cpp"),
    *root.glob("client/api/**/*.h"),
    *root.glob("client/services/*.[ch]"),
    *root.glob("client/services/*.hpp"),
    *root.glob("client/services/*.cpp"),
    ffmpeg_source / "libavutil/tx_template.c",
    ffmpeg_source / "libavcodec/cabac.c",
    ffmpeg_source / "libavcodec/cabac_functions.h",
    root / "patches/ffmpeg/0001-psp-sampled-h264-profile.patch",
    root / "patches/ffmpeg/0002-psp-cabac-movn.patch",
    root / "patches/ffmpeg/0003-psp-approximate-reconstruction.patch",
    ffmpeg_source / "libavcodec/h264_mc_template.c",
    ffmpeg_source / "libavcodec/h264_mb_template.c",
    ffmpeg_source / "libavcodec/h264_mb.c",
    root / "patches/ffmpeg-psp-tx-tables.patch",
    root / "patches/curl-psp-select-pending.patch",
    root / "build/curl-source/lib/select.c",
    *root.glob("decoder/src/*.[ch]"),
    *root.glob("decoder/include/*.h"),
    *root.glob("decoder/me/*"),
    *root.glob("tests/*.hpp"),
    *root.glob("benchmarks/bilibili/*.hpp"),
    *root.glob("third_party/nlohmann/nlohmann/json.hpp"),
    *root.glob("third_party/pystring/pystring.h"),
    root / "CMakeLists.txt",
    root / "Makefile",
    *root.glob("cmake/*.cmake"),
]
libraries = [*ffmpeg.glob("lib/*.a"), curl / "lib/libcurl.a"]
files = {
    entry.name: digest(entry)
    for entry in package.iterdir()
    if entry.is_file() and entry.name != "build.json"
}
manifest = {
    "libraries": {str(path): digest(path) for path in libraries},
    "sources": {
        str(path.relative_to(root) if path.is_relative_to(root) else path): digest(path)
        for path in sources
        if path.is_file()
    },
    "compiler": subprocess.check_output(
        ["psp-gcc", "-dumpfullversion"], text=True
    ).strip(),
    "build": "wiliwili-psp-cpp17-cmake-01",
    "files": files,
    "decoder": "VFPU4 + nearest luma/chroma + list0-only bipred + full residual reconstruction + nonref skip + ME/DMA + audio clock RGB queue; C++17 Wiliwili API/UI frontend",
}
(package / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
