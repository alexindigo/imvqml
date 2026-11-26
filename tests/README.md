# Verification (guest only)

Run in the owned `imvqml-dev` virtual machine on ark, with Qt Test and
bubblewrap installed for `BUILD_TESTING=ON`. `TMPDIR` below is on the guest
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
`installed-scaffold-outcome` also asserts the typed, queued scaffold result.
`installed-pixels-red` is an opt-in acceptance check, not a committed passing
test in the scaffold. Configure with `-DIMV_ENABLE_PIXEL_ACCEPTANCE=ON`, run
`ctest --test-dir build --output-on-failure -R '^installed-pixels-red$'`,
and expect `not-implemented` instead of the two expected RGBA pixels. Reset
the option to `OFF` for normal scaffold verification.
