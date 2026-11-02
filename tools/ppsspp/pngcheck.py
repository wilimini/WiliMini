"""Minimal PNG reader for emulator screenshots (no third-party imports)."""
import struct
import zlib


def png_colors(path):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, idat, width, height, kind = 8, b'', 0, 0, 0
    while pos < len(data):
        length, tag = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if tag == b'IHDR':
            width, height, depth, kind = struct.unpack('>IIBB', body[:10])
        elif tag == b'IDAT':
            idat += body
        pos += 12 + length
    channels = {2: 3, 6: 4}[kind]
    raw = zlib.decompress(idat)
    stride = width * channels
    colors, prior = set(), bytearray(stride)
    for y in range(height):
        base = y * (stride + 1)
        filt, line = raw[base], bytearray(raw[base + 1:base + 1 + stride])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b, c = prior[i], prior[i - channels] if i >= channels else 0
            if filt == 1: line[i] = (line[i] + a) & 255
            elif filt == 2: line[i] = (line[i] + b) & 255
            elif filt == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(width):
            colors.add(bytes(line[x * channels:x * channels + 3]))
        prior = line
    return width, height, len(colors)
