#!/usr/bin/env python3
"""Build client/assets/ime/pinyin.dat, the dictionary of the built-in pinyin input method.

Source: rime-ice (https://github.com/iDvel/rime-ice, GPL-3.0), files cn_dicts/8105.dict.yaml
(single characters with every reading) and cn_dicts/base.dict.yaml (words). The script downloads
them and verifies SHA-256, or reads them from --source-dir. Entries are restricted to characters
the bundled font draws (BMP CJK, U+3400..U+9FFF).

File layout (little endian), read by client/pinyin_ime.cpp through seeks:
  header   "WPYD", u16 version, u16 syllable_count, then for the three tables
           (full, abbreviation, prefix) u32 record_count, u32 index_offset, u32 index_bytes,
           u32 data_offset, u32 data_bytes
  syllables  syllable_count x (u8 length, ascii)
  per table  index: per block u32 data offset (relative to the table data), u8 key length, key
             data:  blocks of up to BLOCK records; record = u8 key length, key, u8 characters,
                    characters x u16 (UTF-16), u16 weight (log scaled)
  full table   key = pinyin without spaces; sorted by key, weight descending
  abbr table   key = first letter of every syllable (words of 2..6 characters)
  prefix table key = 1..3 letter prefix; the best PREFIX_TOP entries whose key is longer
"""
import argparse
import hashlib
import json
import math
import struct
import sys
import urllib.request
from pathlib import Path

SOURCES = {
    '8105.dict.yaml': ('https://raw.githubusercontent.com/iDvel/rime-ice/main/cn_dicts/8105.dict.yaml',
                       '1f9a42b91dea6982baee2551981780271aeffd78876662b9c9f324e56b37b120'),
    'base.dict.yaml': ('https://raw.githubusercontent.com/iDvel/rime-ice/main/cn_dicts/base.dict.yaml',
                       '551cad93c2e74c7325599e0d23afbf71c2d2e57df9af3f8f6fa16d87e60290a3'),
    'LICENSE': ('https://raw.githubusercontent.com/iDvel/rime-ice/main/LICENSE',
                '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'),
}
WORD_LIMIT = 90000
ABBR_LIMIT = 50000
PREFIX_TOP = 10
BLOCK = 32
MAX_WEIGHT = 80000000
VERSION = 1


def fetch(directory):
    for name, (url, digest) in SOURCES.items():
        path = directory / name
        if not path.exists():
            with urllib.request.urlopen(url, timeout=120) as response:
                path.write_bytes(response.read())
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != digest:
            sys.exit(f'{name}: SHA-256 {actual} does not match the pinned source {digest}')


def parse(path):
    body = False
    for line in path.read_text(encoding='utf-8').splitlines():
        if not body:
            body = line == '...'
            continue
        if not line or line.startswith('#'):
            continue
        fields = line.split('\t')
        if len(fields) < 2:
            continue
        weight = int(fields[2]) if len(fields) > 2 and fields[2].isdigit() else 0
        yield fields[0], fields[1].split(), weight


def drawable(text):
    return all(0x3400 <= ord(c) <= 0x9FFF for c in text)


def quantize(weight):
    return round(65535 * math.log1p(min(weight, MAX_WEIGHT)) / math.log1p(MAX_WEIGHT))


def table(records):
    """records: (key, text, weight16) -> (index bytes, data bytes, count)."""
    records = sorted(set(records), key=lambda r: (r[0], -r[2], r[1]))
    index, data = bytearray(), bytearray()
    for start in range(0, len(records), BLOCK):
        chunk = records[start:start + BLOCK]
        key = chunk[0][0].encode()
        index += struct.pack('<IB', len(data), len(key)) + key
        for key, text, weight in chunk:
            raw = key.encode()
            units = text.encode('utf-16-le')
            data += struct.pack('<B', len(raw)) + raw + struct.pack('<B', len(units) // 2) + units
            data += struct.pack('<H', weight)
    return bytes(index), bytes(data), len(records)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source-dir', type=Path, default=None, help='directory holding the rime-ice files')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = root / 'client/assets/ime'
    out.mkdir(parents=True, exist_ok=True)
    source = args.source_dir or Path('/tmp/wili-pinyin-source')
    source.mkdir(parents=True, exist_ok=True)
    fetch(source)

    syllables, chars, words = set(), [], {}  # words: (key, text) -> weight
    for text, readings, weight in parse(source / '8105.dict.yaml'):
        if len(text) == 1 and drawable(text) and len(readings) == 1 and readings[0].isalpha():
            syllables.add(readings[0])
            chars.append((readings[0], text, quantize(weight)))
    for text, readings, weight in parse(source / 'base.dict.yaml'):
        if (2 <= len(text) <= 8 and drawable(text) and len(readings) == len(text) and
                all(r.isascii() and r.isalpha() and r.islower() for r in readings)):
            entry = (''.join(readings), text)
            words[entry] = max(words.get(entry, 0), weight)
    best = sorted(((w, k, t) for (k, t), w in words.items()), reverse=True)[:WORD_LIMIT]
    # The readings of a word are syllables, so they must all be known single-character readings.
    words = [(k, t, quantize(w)) for w, k, t in best]
    full = chars + words

    abbreviations = []
    # Abbreviation key: first letters of the syllables. The syllable split comes from the source
    # readings, so parse again rather than guess from the joined key.
    ranked = []
    for text, readings, weight in parse(source / 'base.dict.yaml'):
        if (2 <= len(text) <= 6 and drawable(text) and len(readings) == len(text) and
                all(r.isascii() and r.isalpha() and r.islower() for r in readings)):
            ranked.append((weight, text, ''.join(r[0] for r in readings), ''.join(readings)))
    keep = {(k, t) for _, k, t in best}
    ranked = [r for r in ranked if (r[3], r[1]) in keep]
    ranked.sort(reverse=True)
    for weight, text, abbreviation, _ in ranked[:ABBR_LIMIT]:
        abbreviations.append((abbreviation, text, quantize(weight)))

    pools = {}
    for key, text, weight in full:
        for n in (1, 2, 3):
            if len(key) > n:
                pools.setdefault(key[:n], []).append((weight, key, text))
    prefix = []
    for key, pool in pools.items():
        for weight, _, text in sorted(pool, reverse=True)[:PREFIX_TOP]:
            prefix.append((key, text, weight))

    tables = [table(full), table(abbreviations), table(prefix)]
    syl_blob = b''.join(struct.pack('<B', len(s)) + s.encode() for s in sorted(syllables))
    header_size = 8 + 3 * 20
    offset = header_size + len(syl_blob)
    header = b'WPYD' + struct.pack('<HH', VERSION, len(syllables))
    body = b''
    for index, data, count in tables:
        header += struct.pack('<IIIII', count, offset, len(index), offset + len(index), len(data))
        body += index + data
        offset += len(index) + len(data)
    blob = header + syl_blob + body
    (out / 'pinyin.dat').write_bytes(blob)
    (out / 'MANIFEST.json').write_text(json.dumps({
        'source': 'rime-ice cn_dicts/8105.dict.yaml and base.dict.yaml',
        'license': 'GPL-3.0-or-later (see LICENSE-rime-ice)',
        'source_sha256': {k: v[1] for k, v in SOURCES.items()},
        'format_version': VERSION,
        'syllables': len(syllables),
        'full_records': tables[0][2], 'abbreviation_records': tables[1][2], 'prefix_records': tables[2][2],
        'bytes': len(blob),
        'generator': 'scripts/prepare-pinyin-dict.py',
    }, indent=2) + '\n')
    (out / 'LICENSE-rime-ice').write_bytes((source / 'LICENSE').read_bytes())
    print(out / 'pinyin.dat', len(blob), 'bytes;', len(syllables), 'syllables;',
          [t[2] for t in tables], 'records')


main()
