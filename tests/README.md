# Verification (guest only)

Run in the owned `imvqml-dev` virtual machine on ark, with Qt Test and
bubblewrap, Python 3, libzstd and ImageMagick with the listed codec writers
installed for `BUILD_TESTING=ON`. `TMPDIR` below is on the guest
disk, outside both the original source and build trees:

```sh
cd /home/tester/Projects/imvqml-dev
export TMPDIR=$PWD/tmp
cmake -S src -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build --parallel 4
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build --output-on-failure -LE wayland
```

`installed-prefix` builds external C++ and QML consumers against a moved
install prefix. Bubblewrap overlays both original trees with empty read-only
directories; the QML consumer checks the relocated paths in `/proc/self/maps`.
`installed-sample` asserts the two expected RGBA pixels under the same
isolation. `jobs` uses generated fixture files and no display.
`pipeline` proves that dropping a listener after admission does not deliver a
stale outcome or cancel another listener's still-active request.

With `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` set to the verified guest niri
session, run `QT_QPA_PLATFORM=wayland ctest --test-dir build --output-on-failure
-L wayland` on one line. `items` requires a presented Qt Quick window and
reads back its rendered pixels for fill, masks, resize and source changes.
Private counters prove decode and texture sharing and reuse.
The same two lanes check exact GIF/WebP frame delays, elapsed playback and
finite loops, and dedicated playback-helper pause/resume with a large GIF.

To rerun the persistent-format measurement inside the guest niri session,
use `QT_QPA_PLATFORM=wayland build/tests/imv-store-benchmark` with the
guest's `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` set. The benchmark checks
each codec's exact RGBA round trip before reading the encoded files and
uploading to OpenGL textures. The cold lane advises eviction from the guest
page cache, not any virtual-disk or host cache.
