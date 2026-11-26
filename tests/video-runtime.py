#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
import pathlib
import subprocess
import sys
import tempfile

bwrap, runtime, executable, mode, fake = sys.argv[1:]
runtime = pathlib.Path(runtime).resolve(strict=True)
base = [bwrap, '--die-with-parent', '--unshare-net', '--ro-bind', '/', '/',
        '--dev', '/dev', '--proc', '/proc', '--unsetenv', 'LD_LIBRARY_PATH',
        '--setenv', 'IMV_VIDEO_TEST', mode, '--setenv', 'IMV_MPV_FAULT_MODE', mode]
if mode != 'missing':
    # Override the resolved file, so both the loader's SONAME and an explicit
    # non-FHS hint open the same fixture without changing production discovery.
    subprocess.run([*base, '--ro-bind', fake, runtime, '--', executable,
                    'watchdogWithoutGui' if mode == 'watchdog' else 'containment'], check=True)
    sys.exit(0)

library = runtime.parent
with tempfile.TemporaryDirectory(prefix='imv-no-mpv-') as tmp:
    overlay = pathlib.Path(tmp, 'overlay')
    overlay.mkdir()
    real = pathlib.Path(tmp, 'original')
    real.mkdir()
    for original in library.iterdir():
        if not original.name.startswith('libmpv.so') and original != runtime:
            (overlay / original.name).symlink_to(real / original.name)
    subprocess.run([*base, '--ro-bind', library, real, '--ro-bind', overlay, library,
                    '--', executable, 'containment'], check=True)
