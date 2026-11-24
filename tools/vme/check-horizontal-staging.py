#!/usr/bin/env python3
"""Check experimental horizontal FIR staging using mcidclan's vmee emulator.

Usage: python3 tools/vme/check-horizontal-staging.py /path/to/vme2-next-level
This validates the first pass only, not hardware DMA or the complete FIR.
"""
import random
import sys
sys.path.insert(0, sys.argv[1])
from vmee.machine import Emulator

for width in (16, 640):
    rng = random.Random(12345)
    rows = [[rng.randrange(256) for _ in range(width + 5)],
            [0] * (width + 5), [255] * (width + 5),
            [255 * (i % 2) for i in range(width + 5)]]
    for row in rows:
        emu = Emulator()
        for i, value in enumerate(row):
            emu.scratchpad.write(0x8000 + i, value)
        regs = {
            33: 0x84000000, 34: 0x10000 | (width + 4),
            28: 0x44404440, 29: 0x66666666,
            0: 0x4000, 1: 0x4000,
            2: 0x80000000 | (0x010 << 12),
            6: 0x04800000 | 0x00204000, 21: 5,
            45: 0x84060000, 46: 0x10000 | (width + 4),
            63: 0x84060000, 64: 0x10000 | (width + 4),
            81: 0x840d0000, 82: 0x10000 | (width - 1),
            30: 0x42104210, 27: 0x20000000,
        }
        for index, value in regs.items():
            emu.registers[index] = value
        emu.trigger()
        for bank in (0, 0x800):
            actual = [emu.scratchpad.read(bank + i) for i in range(width + 5)]
            assert actual == row, (width, bank, actual[:8], row[:8])
        actual = [emu.scratchpad.read(0x1000 + i) for i in range(width)]
        expected = [5 * (row[x+1] + row[x+4]) for x in range(width)]
        assert actual == expected, (width, actual[:8], expected[:8])
print('Horizontal staging pass: 8 cases passed')
