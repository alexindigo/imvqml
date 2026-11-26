# imvqml

Qt 6 / QML image pipeline for Linux. This initial checkpoint installs the
module, CMake package and decoder process skeleton; it does not decode images
yet. See [API](API.md), [architecture](Architecture.md),
[feature mapping](FeatureMapping.md) and [upstream notes](UPSTREAM.md).

Build and install with CMake, a C++20 compiler and Qt 6.8 or newer. All
development builds and tests run inside the owned project VM. Set
`BUILD_TESTING=OFF` for a build without test-only dependencies.

Install the formatting and whitespace git hook with
`install -m755 scripts/pre-commit .git/hooks/pre-commit`.

License: LGPL-3.0-only, see [LICENSE](LICENSE).
