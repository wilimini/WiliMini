#!/usr/bin/env python3
import argparse
import json
import re
from pathlib import Path


def cpp_string(value):
    return json.dumps(value, ensure_ascii=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--zh-hans", required=True, type=Path)
    parser.add_argument("--en-us", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    catalogs = {
        "zh-Hans": json.loads(args.zh_hans.read_text(encoding="utf-8")),
        "en-US": json.loads(args.en_us.read_text(encoding="utf-8")),
    }
    keys = sorted(catalogs["zh-Hans"])
    if keys != sorted(catalogs["en-US"]):
        raise SystemExit("Localization catalogs must contain the same keys")

    identifiers = {}
    for key in keys:
        if not re.fullmatch(r"[a-z][a-z0-9_]*", key):
            raise SystemExit(f"Invalid localization key: {key}")
        identifiers[key] = key
        zh = catalogs["zh-Hans"][key]
        en = catalogs["en-US"][key]
        if not isinstance(zh, str) or not isinstance(en, str):
            raise SystemExit(f"Localization values must be strings: {key}")
        zh_args = sorted(re.findall(r"\{([a-z][a-z0-9_]*)\}", zh))
        en_args = sorted(re.findall(r"\{([a-z][a-z0-9_]*)\}", en))
        if zh_args != en_args:
            raise SystemExit(f"Placeholder mismatch: {key}")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    header = ["#pragma once", "#include <cstddef>", "#include <cstdint>", "", "namespace pspapp {",
              "enum class TextId : std::uint16_t {"]
    header.extend(f"    {identifiers[key]}," for key in keys)
    header.extend(["    Count", "};", "", "extern const char* const kTranslationsZhHans[];",
                   "extern const char* const kTranslationsEnUs[];", "} // namespace pspapp", ""])
    (args.output_dir / "localization.generated.hpp").write_text("\n".join(header), encoding="utf-8")

    source = ['#include "localization.generated.hpp"', "", "namespace pspapp {",
              "const char* const kTranslationsZhHans[] = {"]
    source.extend(f"    {cpp_string(catalogs['zh-Hans'][key])}," for key in keys)
    source.extend(["};", "", "const char* const kTranslationsEnUs[] = {"])
    source.extend(f"    {cpp_string(catalogs['en-US'][key])}," for key in keys)
    source.extend(["};", "} // namespace pspapp", ""])
    (args.output_dir / "localization.generated.cpp").write_text("\n".join(source), encoding="utf-8")


if __name__ == "__main__":
    main()
