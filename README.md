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

Install the formatting and whitespace git hook with
`install -m755 scripts/pre-commit .git/hooks/pre-commit`.

License: LGPL-3.0-only, see [LICENSE](LICENSE).
