# imvqml

Image display, thumbnails, and headless image jobs for Qt 6 applications on
Linux. `ImvImage` is a Qt Quick item with shared textures, crop and mask
controls, and animated GIF and WebP playback. `ImvImageJob` inspects images,
samples pixels, and exports PNG or WebP without a window. `ImvCache` manages
an optional disk cache and exports freedesktop thumbnails.

The decoder supports JPEG, PNG, GIF, WebP, TIFF, SVG, HEIF, AVIF, and JPEG XL
when the corresponding libvips loaders are available. Use local paths,
`file:` or `data:` URLs, or Qt image providers for display. The module does not
fetch network URLs.

## Usage

Save this as `Preview.qml`, put `sample.jpg` beside it, and open it with your
Qt 6 QML runner:

```qml
import QtQuick
import ImvQml

ImvImage {
    id: preview
    width: 480
    height: 320
    source: Qt.resolvedUrl("sample.jpg").toString()
    fillMode: ImvImage.Cover
    mask: ImvImage.Rounded
    radius: 12

    ImvImageJob {
        id: metadata
        onFinished: function(ticket, result) {
            if (result.stage === "complete")
                console.log(result.sourceInfo.width + " x " + result.sourceInfo.height)
        }
    }

    Component.onCompleted: {
        ImvCache.enabled = true
        metadata.inspect(preview.source)
    }
}
```

Set `animate: false` for a first-frame preview. With the optional libmpv
adapter, you can also use a local video as the source and set `videoPosition`
to a fraction from `0` to `1`. This captures a still frame; it does not play
video or audio. Image decoding works without libmpv installed.

## Why a separate decoder?

Encoded media goes to a separate helper that returns pixels to the app. If a
codec crashes or hangs, imvqml can report the request failure and retire the
helper instead of losing the QML process. The helper restricts which libvips
loaders it uses, and request sizes and animation buffering are bounded. This
is fault isolation, not an OS sandbox.

## Requirements

- Linux, a C++20 compiler, CMake 3.24+, and pkg-config.
- Qt 6.8+ (Core, Gui, Qml, Quick), libvips 8.14+, and libwebp. AVIF/HEIF and
  JPEG XL need libvips built with the appropriate libheif and libjxl loaders.
- Video-capable builds (the default) require mpv 0.35+ development headers
  exposing client ABI 2 and software rendering. At runtime only the decoder
  loads `libmpv.so.2`; still images work without it. An unavailable or
  incompatible backend is reported as `inspect/unsupported-format` with detail
  `video-support-unavailable`. Set `-DIMV_ENABLE_VIDEO=OFF` to omit the adapter.

## Build & Install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --parallel
sudo cmake --install build --prefix /usr
```

This installs the `ImvQml` module, backing library, and `imvqml-decoder`
helper. Keep all three from the same build in the same prefix. For a custom
prefix, add `<prefix>/lib/qt6/qml` to the QML import path (adjust `lib` to
match `CMAKE_INSTALL_LIBDIR`). C++ consumers can use
`find_package(imvqml 0.1 CONFIG REQUIRED)` and link `imvqml::imvqml`; set
`CMAKE_PREFIX_PATH` for a custom prefix.

For development builds and installed-module checks, see
[tests/README.md](tests/README.md). In a pinned nixpkgs environment,
`pkgs.callPackage ./package.nix { }` builds with the adapter and build-only mpv
headers. The default output does not include libmpv in its runtime closure;
the guest loader must be able to find it (for example through `LD_LIBRARY_PATH`).
Pass `withVideo = true` to pin libmpv in the runtime closure. Non-FHS CMake
packages can set `IMV_MPV_RUNTIME_HINT` to select an absolute library path in
place of the default loader search.

## Documentation

| File | Contents |
|------|----------|
| [API.md](API.md) | QML and C++ types, job results, caching, and thumbnails |
| [Architecture.md](Architecture.md) | Decoder, transport, cache, and rendering |
| [FeatureMapping.md](FeatureMapping.md) | Feature and test coverage |
| [UPSTREAM.md](UPSTREAM.md) | Upstream references and provenance |

License: LGPL-3.0-only. See [LICENSE](LICENSE).
