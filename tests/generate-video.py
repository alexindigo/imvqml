#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
import pathlib
import subprocess
import sys

root = pathlib.Path(sys.argv[1])


def run(*args):
    subprocess.run([sys.argv[2], '-hide_banner', '-loglevel', 'error', '-y',
                    '-filter_threads', '1', '-filter_complex_threads', '1', *args],
                   cwd=root, check=True)


run('-f', 'lavfi', '-i', 'color=red:s=128x72:r=10:d=2',
    '-f', 'lavfi', '-i', 'color=blue:s=128x72:r=10:d=2', '-filter_complex',
    '[0:v][1:v]concat=n=2:v=1:a=0', '-threads', '1', '-c:v', 'libvpx-vp9', 'seek.webm')
run('-f', 'lavfi', '-i', 'color=red:s=128x72:r=10:d=1', '-vf',
    'drawbox=x=64:y=0:w=64:h=36:color=lime:t=fill,'
    'drawbox=x=0:y=36:w=64:h=36:color=blue:t=fill,'
    'drawbox=x=64:y=36:w=64:h=36:color=yellow:t=fill',
    '-threads', '1', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', 'rotation-base.mp4')
for angle in (90, 180, 270):
    run('-display_rotation', str(angle), '-i', 'rotation-base.mp4', '-c', 'copy', f'rotation-{angle}.mp4')
