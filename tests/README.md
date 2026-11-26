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

`installed-prefix`, `installed-sample` and `installed-wayland` build external
consumers against a moved install prefix. Bubblewrap overlays both original
trees with empty read-only directories. The probes check the relocated
paths in `/proc/self/maps` and exact decoded or rendered pixels. The QML
consumers link Qt only, not imvqml. `package-metadata` checks the local CPack
archive inventory, then runs the headless consumers on the extracted archive.
Use `cmake -DARTIFACT_PREFIX=/absolute/prefix -DPIXELS=ON -DWAYLAND=ON
-P build/tests/relocate.cmake` on one line to run these probes against a
package prefix outside the source and build trees. Use the package's Qt
SDK and matching GNUInstallDirs layout when configuring the probe runner.
For split Qt SDKs, pass the SDK's imports directory as `QT_SDK_QML_PATH`.
`jobs` uses generated fixture files and no display.
`pipeline` proves that dropping a listener after admission does not deliver a
stale outcome or cancel another listener's still-active request.

With `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` set to the verified guest niri
session, run `QT_QPA_PLATFORM=wayland ctest --test-dir build --output-on-failure
-L wayland` on one line. `items` requires a presented Qt Quick window and
reads back its rendered pixels for fill, masks, resize and source changes.
Private counters prove decode and texture sharing and reuse.
The same two lanes check exact GIF/WebP frame delays, elapsed playback and
finite loops, and dedicated playback-helper pause/resume with a large GIF.

Use `-DIMV_REQUIRE_VIDEO_TESTS=ON` in release verification to prevent a
missing runtime from silently dropping the native tests. It also accepts
the explicit `IMV_MPV_RUNTIME_HINT` used by the decoder. The tests need
FFmpeg with libvpx and libx264 for seek and rotation fixtures. Isolated
tests replace or hide the runtime and verify startup errors, native abort,
the helper watchdog, and continued still-image decoding. With
`IMV_ENABLE_VIDEO=OFF`, `video-disabled` checks the compile-time opt-out.

To rerun the persistent-format measurement inside the guest niri session,
use `QT_QPA_PLATFORM=wayland build/tests/imv-store-benchmark` with the
guest's `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` set. The benchmark checks
each codec's exact RGBA round trip before reading the encoded files and
uploading to OpenGL textures. The cold lane advises eviction from the guest
page cache, not any virtual-disk or host cache.
