#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
import pathlib
import struct
import subprocess
import sys
import zlib

root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)


def convert(*args):
    subprocess.run([sys.argv[2], '-limit', 'thread', '2', *args], cwd=root, check=True)


def chunk(kind, data):
    return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))


def png(width, height, raw):
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


width, height = 64, 32
row = b'\0' + bytes.fromhex('ff0000ff') * (width // 2) + bytes.fromhex('00ff00ff') * (width // 2)
(root / 'palette.png').write_bytes(png(width, height, row * height))
for ext in ('jpg', 'gif', 'webp', 'tiff', 'heic', 'avif', 'jxl', 'ppm'):
    convert('palette.png', '-quality', '95', f'palette.{ext}')
(root / 'palette.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="64" height="32">
<rect width="32" height="32" fill="#ff0000"/><rect x="32" width="32" height="32" fill="#00ff00"/>
</svg>''')

convert('palette.png', '-sampling-factor', '4:4:4', '-quality', '98', 'orientation-base.jpg')
jpeg = (root / 'orientation-base.jpg').read_bytes()
# Minimal EXIF IFD0: Orientation 6 (clockwise 90 degrees).
exif = b'Exif\0\0II' + struct.pack('<HIH', 42, 8, 1) + struct.pack('<HHIHHI', 0x112, 3, 1, 6, 0, 0)
(root / 'orientation.jpg').write_bytes(jpeg[:2] + b'\xff\xe1' + struct.pack('!H', len(exif) + 2) + exif + jpeg[2:])
(root / 'alpha.png').write_bytes(png(2, 1, bytes.fromhex('00ff00008000ff0040')))
(root / 'huge.png').write_bytes(png(16384, 8192, b'\0'))
(root / 'broken.png').write_bytes(b'\x89PNG\r\n\x1a\n' + b'not a header')
(root / 'garbage').write_bytes(b'not an image')
cards = ['SIMPLE  =                    T', 'BITPIX  =                    8', 'NAXIS   =                    2',
         'NAXIS1  =                    2', 'NAXIS2  =                    1', 'END']
(root / 'palette.fits').write_bytes(''.join(card.ljust(80) for card in cards).encode().ljust(2880, b' ')
                                    + b'\x00\xff'.ljust(2880, b'\x00'))
with (root / 'large-source').open('wb') as stream:
    stream.truncate(256 * 1024 * 1024 + 1)
