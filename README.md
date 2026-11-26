# imvqml

Qt 6 / QML image pipeline for Linux. `ImvImage` displays images with shared
textures; `ImvImageJob` inspects, samples or exports local images without a
display. Source decoding runs in a separate
libvips helper. See [API](API.md), [architecture](Architecture.md),
[feature mapping](FeatureMapping.md) and [upstream notes](UPSTREAM.md).

Build and install with CMake, a C++20 compiler, Qt 6.8 or newer, libvips
with the documented loaders, and libwebp. All
development builds and tests run inside the owned project VM. Set
`BUILD_TESTING=OFF` for a build without test-only dependencies.

Optional video capture uses mpv headers at build time and loads
`libmpv.so.2` only inside the helper at runtime. It works without the DSO
installed for images. `-DIMV_ENABLE_VIDEO=OFF` disables the adapter.

## Use from an installed prefix

For QML, add `<prefix>/lib/qt6/qml` to the engine import path (adjust `lib`
if the package uses a different library directory).

```qml
import QtQuick
import ImvQml

ImvImage {
    width: 240
    height: 160
    source: "file:///home/user/Pictures/sample.jpg"
    fillMode: ImvImage.Cover
    mask: ImvImage.Rounded
    radius: 12
}
```

For C++, use `find_package(imvqml 0.1 CONFIG REQUIRED)`, then link your
target to `imvqml::imvqml`. Add the prefix to `CMAKE_PREFIX_PATH` if needed.
Keep the QML plugin, backing library, and decoder from the same build in
that prefix. CMake installs these files and can create a local TGZ via CPack.
In a pinned nixpkgs environment, `pkgs.callPackage ./package.nix { }` defines
the package, with `withVideo = true` as an opt-in. No package is published here.

Install the formatting and whitespace git hook with
`install -m755 scripts/pre-commit .git/hooks/pre-commit`.

License: LGPL-3.0-only, see [LICENSE](LICENSE).
