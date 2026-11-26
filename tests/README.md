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
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build --output-on-failure
```

`installed-prefix` builds external C++ and QML consumers against a moved
install prefix. Bubblewrap overlays both original trees with empty read-only
directories; the QML consumer checks the relocated paths in `/proc/self/maps`.
`installed-sample` asserts the two expected RGBA pixels under the same
isolation. `jobs` uses generated fixture files and no display.

To rerun the persistent-format measurement inside the guest niri session,
use `QT_QPA_PLATFORM=wayland build/tests/imv-store-benchmark` with the
guest's `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` set. The benchmark checks
each codec's exact RGBA round trip before reading the encoded files and
uploading to OpenGL textures. The cold lane advises eviction from the guest
page cache, not any virtual-disk or host cache.
