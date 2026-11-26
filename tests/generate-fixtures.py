#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
import pathlib
import gzip
import struct
import subprocess
import sys
import zlib

from PIL import Image

root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)


def vips_copy(source, destination):
    subprocess.run([sys.argv[2], 'copy', source, destination, '--vips-concurrency=2'],
                   cwd=root, check=True)


def chunk(kind, data):
    return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))


def png(width, height, raw):
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


width, height = 64, 32
row = b'\0' + bytes.fromhex('ff0000ff') * (width // 2) + bytes.fromhex('00ff00ff') * (width // 2)
(root / 'palette.png').write_bytes(png(width, height, row * height))
for ext, options in [('jpg', '[Q=95]'), ('gif', ''), ('webp', '[lossless=true]'),
                     ('tiff', ''), ('heic', '[Q=95]'), ('avif', '[Q=95]'), ('jxl', '[Q=95]')]:
    vips_copy('palette.png', f'palette.{ext}{options}')
colors = ['red', 'lime', 'yellow']
frames = [Image.new('RGB', (64, 32), color) for color in colors]
for ext, loop in [('gif', 1), ('webp', 2)]:
    frames[0].save(root / f'animation.{ext}', save_all=True, append_images=frames[1:],
                   duration=[200, 300, 450], loop=loop, lossless=True)

# 18 * 1024 * 1024 * 4 exceeds the decoded frame budget; the encoded GIF is small.
frames = [Image.new('RGB', (1024, 1024), color) for color in colors] * 6
frames[0].save(root / 'stream.gif', save_all=True, append_images=frames[1:], duration=250, loop=0)
(root / 'palette.svg').write_text('''<svg xmlns="http://www.w3.org/2000/svg" width="64" height="32">
<rect width="32" height="32" fill="#ff0000"/><rect x="32" width="32" height="32" fill="#00ff00"/>
</svg>''')

# Generate the additional fixtures without using ImageMagick. Aliases use
# the same real encoded format with their customary filename extension.
def put(name, data):
    (root / name).write_bytes(data)


def read(name):
    return (root / name).read_bytes()


for alias, original in [('jpeg', 'jpg'), ('tif', 'tiff'), ('heif', 'heic')]:
    put(f'palette.{alias}', read(f'palette.{original}'))
put('palette.svgz', gzip.compress(read('palette.svg'), mtime=0))
bgr = (b'\x00\x00\xff' * 32 + b'\x00\xff\x00' * 32) * height
dib = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 24, 0, len(bgr), 0, 0, 0, 0)
put('palette.bmp', struct.pack('<2sIHHI', b'BM', 54 + len(bgr), 0, 0, 54) + dib + bgr)
square = png(64, 64, row * 64)
put('palette.ico', struct.pack('<HHHBBBBHHII', 0, 1, 1, 64, 64, 0, 0, 1, 32, len(square), 22)
    + square)
put('palette.icns', struct.pack('>4sI4sI', b'icns', 16 + len(square), b'ic12', 8 + len(square))
    + square)
tga = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, width, height, 24, 32)
put('palette.tga', tga + bgr + bytes(8) + b'TRUEVISION-XFILE.\x00')
dds = b'DDS ' + struct.pack('<31I', 124, 0x100f, height, width, width * 4, 0, 0, *([0] * 11),
                           32, 0x41, 0, 32, 0xff0000, 0xff00, 0xff, 0xff000000, 0x1000, 0, 0, 0, 0)
put('palette.dds', dds + (b'\x00\x00\xff\xff' * 32 + b'\x00\xff\x00\xff' * 32) * height)
ppm = b'P6\n64 32\n255\n' + (b'\xff\x00\x00' * 32 + b'\x00\xff\x00' * 32) * height
put('palette.ppm', ppm)
put('palette.pnm', ppm)
put('palette.pgm', b'P5\n64 32\n255\n' + (b'\xff' * 32 + b'\x00' * 32) * height)
put('palette.pbm', b'P4\n64 32\n' + (bytes(4) + b'\xff' * 4) * height)
put('palette.hdr', b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 32 +X 64\n'
    + (b'\x80\x00\x00\x81' * 32 + b'\x00\x80\x00\x81' * 32) * height)
for ext, encoder, extra in [('jp2', 'jpeg2000', ['-format', 'jp2']),
                            ('j2k', 'jpeg2000', ['-format', 'j2k']),
                            ('exr', 'exr', ['-pix_fmt', 'gbrpf32le'])]:
    subprocess.run([sys.argv[3], '-loglevel', 'error', '-y', '-i', 'palette.png', '-frames:v', '1',
                    '-c:v', encoder, '-threads', '1', *extra, '-f', 'image2', f'palette.{ext}'],
                   cwd=root, check=True)

# Keep enough identification bytes to select the intended backend, but not
# enough of the header to construct a usable image.
for ext, length in [('bmp', 2), ('ico', 6), ('icns', 8), ('dds', 4),
                    ('exr', 4), ('hdr', 12), ('jp2', 12), ('ppm', 10)]:
    put(f'broken.{ext}', read(f'palette.{ext}')[:length])
put('broken.tga', bytes(26) + b'TRUEVISION-XFILE.\x00')
put('broken.svgz', gzip.compress(b'<svg xmlns="http://www.w3.org/2000/svg"', mtime=0))

vips_copy('palette.png', 'orientation-base.jpg[Q=98,subsample-mode=off]')
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
    stream.write(read('palette.avif'))
    stream.truncate(256 * 1024 * 1024 + 1)
