#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
import os
import pathlib
import subprocess
import sys
import tempfile

bwrap, library, executable = sys.argv[1:]
library = pathlib.Path(library).resolve().parent
assert any(library.glob('libmpv.so*'))
with tempfile.TemporaryDirectory(prefix='imv-no-mpv-') as tmp:
    overlay = pathlib.Path(tmp, 'overlay')
    overlay.mkdir()
    real = pathlib.Path(tmp, 'original')
    real.mkdir()
    for original in library.iterdir():
        if not original.name.startswith('libmpv.so'):
            (overlay / original.name).symlink_to(real / original.name)
    subprocess.run([bwrap, '--unshare-net', '--ro-bind', '/', '/', '--dev', '/dev', '--proc', '/proc',
                    '--ro-bind', library, real, '--ro-bind', overlay, library,
                    '--unsetenv', 'LD_LIBRARY_PATH', '--setenv', 'IMV_VIDEO_TEST', 'missing',
                    '--', executable, 'containment'], check=True)
