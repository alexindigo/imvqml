# Upstream sources and dependencies

- mpvqml v0.3.0 (`56a1fc63686e6c87b46cd6d75f79622b81cca757`), LGPL-3.0-only:
  discovery, producer and installed-consumer architecture reference. The
  license files are copied from that tag. No capture code is ported yet.
- Qt 6.8 or newer: QML registration and descriptor-preserving process
  setup (`QProcess::setChildProcessModifier`, `UnixProcessParameters`).
  `QProcess` provides nonblocking event-loop reaping; the private pool keeps
  each live process object out of synchronous destruction.
- The Linux helper caps address space at 1 GiB (`RLIMIT_AS`) and disables
  core dumps. It is not a general OS sandbox.
- libvips 8.18.7 verified, with JPEG XL via libjxl 0.12.0, HEIF/AVIF via
  libheif 1.23.6, SVG via librsvg, GIF via nsgif, and distro PNG, JPEG, WebP
  and TIFF loaders. `VIPS_OPERATION_UNTRUSTED` means the upstream operation
  depends on code not treated as hardened for untrusted input. libvips marks
  its external-libjxl JPEG XL loader (and its librsvg SVG loader) this way.
  The exact source operations are enabled by the approved allow-list in the
  helper only; their filename/buffer variants remain blocked. This does not
  change upstream's security classification or remove the decoder's limits.
- libwebp encodes trusted normalized RGBA for optional lossless file export;
  source decoding stays in the helper. No libmpv linkage yet.
- The store measurement uses the MIT-licensed QOI reference header from
  phoboslab/qoi commit `ffb2d2cb74a1de60819b21b939f7209aa53e91c1`, in
  `tests/vendor/qoi.h` with one blank line's trailing tabs removed. It is
  also used by the private store; the reader checks chunk bounds, exact
  pixel count, geometry, payload checksum and marker before decoding.
- The display path uses Qt Quick's public scene-graph APIs. `QQuickWindow`
  creates the texture at scene-graph sync and signals invalidation on the
  render thread; no Qt private API or platform GL context is used. Verified
  with Qt 6.12's threaded OpenGL RHI in the guest niri session.
- Animation uses the allowed libvips loaders' `delay` and `loop` metadata
  and `page` selection for one composited frame. A source with pages but no
  complete delay table remains a still. GIF and WebP were verified.
- Thumbnail export uses the freedesktop.org Thumbnail Managing Standard's
  URI-derived PNG filename, size buckets and required URI/mtime text fields.
