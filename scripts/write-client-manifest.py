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
parser.add_argument("--curl", required=True, type=Path)
args = parser.parse_args()

root = args.root.resolve()
package = args.package.resolve()
ffmpeg = args.ffmpeg.resolve()
curl = args.curl.resolve()

sources = [
    *root.glob("client/*.[ch]"),
    *root.glob("client/*.hpp"),
    *root.glob("client/*.cpp"),
    *root.glob("client/api/**/*.h"),
    *root.glob("client/services/*.[ch]"),
    *root.glob("client/services/*.hpp"),
    *root.glob("client/services/*.cpp"),
    root / "third_party/ffmpeg/libavutil/tx_template.c",
    root / "patches/ffmpeg-psp-tx-tables.patch",
    root / "scripts/build-audio-libs.sh",
    root / "patches/curl-psp-select-pending.patch",
    root / "scripts/build-client-curl.sh",
    root / "build/curl-source/lib/select.c",
    *root.glob("decoder/src/*.[ch]"),
    *root.glob("decoder/include/*.h"),
    *root.glob("decoder/me/*"),
    *root.glob("third_party/nlohmann/nlohmann/json.hpp"),
    *root.glob("third_party/pystring/pystring.h"),
    root / "CMakeLists.txt",
    *root.glob("cmake/*.cmake"),
    root / "scripts/build-client.sh",
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
        str(path.relative_to(root)): digest(path)
        for path in sources
        if path.is_file()
    },
    "compiler": subprocess.check_output(
        ["psp-gcc", "-dumpfullversion"], text=True
    ).strip(),
    "build": "wiliwili-psp-cpp17-cmake-01",
    "files": files,
    "decoder": "VFPU4 + bilinear qpel + nonref skip + ME/DMA + audio clock RGB queue; C++17 Wiliwili API/UI frontend",
}
(package / "build.json").write_text(json.dumps(manifest, indent=2) + "\n")
