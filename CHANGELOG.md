# Changelog

## [0.1.3] - 2026-11-26

### Fix

The isolated decoder restores compatibility with BMP, Netpbm, ICO, ICNS,
JPEG 2000, OpenEXR, Radiance HDR, DDS, and TGA without using ImageMagick.
Local videos above 256 MiB can supply thumbnails without copying the file;
image size limits and video capture limits still apply. Cache clear and
invalidation now return tickets and report what was removed, kept under a
live lease, or could not be removed.

- Restore media coverage and report cache eviction (`00af407`, merged in `3b7e97d`).

## [0.1.2] - 2026-11-26

### Fix

`ImvImageJob` captures transient provider pixels and owns file or data inputs
without waiting for helper capacity. Explicit jobs return a cancellable
ticket and use the shared admission queue; only a full queue returns `-2`
without capturing the input. Thumbnail exports open the source at call time
and never publish against a replaced file.

- Queue explicit image jobs and capture inputs at call time (`a6b7727`, merged in `bc711eb`).

## [0.1.1] - 2026-11-26

### Fix

`ImvImage` stays loading during bursts instead of failing with `busy`.
The pipeline prioritizes visible items, drops superseded requests, and
reports `deadline` when queued work times out or cannot fit. Thumbnail export
also queues for writer capacity. Explicit `ImvImageJob` calls still return
`-2` when busy.

- Queue image display and thumbnail admission (`ba1dd65`).

## [0.1.0] - 2026-11-26

### Fix

Image providers can discard a transient image immediately after giving its URL
to an item or job. imvqml takes ownership of the pixels at assignment or when
an async response arrives, so resizing does not depend on the provider keeping
the original pixels around. Circle and rounded masks blend at the edges on
both default and software Qt Quick renderers. Sealed source and output bounds,
deadline and ownership checks, and descriptor-based export carry those
same guarantees through the helper, cache, presentation and publication.
An incompatible video backend is reported separately from corrupt media.

- Snapshot Qt image provider pixels at the source boundary (`2099424`).
- Rasterize image masks into antialiased alpha (`d2fc450`).

- Own borrowed pixels before retiring Qt provider textures (`b8630a4`).
- Bound the shared output descriptor before helper handoff (`0f4e6c9`).
- Preserve invalidation through cache directory lock contention (`a966592`).
- Decode only the first frame for static previews (`dc5ebe4`).
- Prepare unmasked tint textures at decoded-frame size (`d248067`).
- Report invalid positions and exhausted tickets separately (`18406b5`).
- Bind classification, decode and cache identity to the same sealed source (`1762644`).
- Bound source preparation and end-to-end work, including stalled helpers (`a7357c3`).
- Track texture update outcomes and limit retained frame backing (`6a2c554`).
- Publish exported images from the staged inode without clobbering (`3849a5b`).
- Require a usable video SDK and distinguish backend availability from media errors (`0680c5f`).

### Feature

The first release adds a Qt 6/QML image pipeline for Linux. It runs media
decoding out of process and exposes image display, headless jobs, animation,
and optional video-frame capture. A private pixel store and freedesktop
thumbnail export let callers reuse results without decoding again. Display
tint recolors symbolic icons without changing the source alpha or decoding again.

- Scaffold the QML module and decoder transport (`25c7319`).
- Bound decoder jobs and retire failed helpers (`960a730`).
- Decode sealed pixels and expose headless image jobs (`03eb3fe`).
- Display images with shared scene-graph textures (`4bcaa3e`).
- Play animations with bounded frame buffering (`1543f63`).
- Persist decoded frames and export freedesktop thumbnails (`db3fe44`).
- Capture optional video frames in the helper (`4737424`).
- Tint image textures without changing source pixels (`611c493`).

### Packaging and documentation

The install layout keeps the QML plugin, backing library, and helper together.
Installed-consumer tests verify that the library still finds its helper after
the prefix moves or an archive is extracted. The probe now checks
installed tint coverage and rejects direct ELF links to the optional video
runtime. The README shows how to build and use the module.

- Verify relocated installs and local packages (`f3e7b6f`).
- Document installation and QML usage (`ea6dfc8`).
- Run relocated consumers against extracted packages and check installed tint (`337e298`).
