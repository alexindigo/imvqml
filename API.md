# imvqml API

Import `ImvQml` from QML. For a custom prefix, add its `lib/qt6/qml`
directory to the QML import path (replace `lib` with CMake's install libdir).
C++ consumers use `find_package(imvqml 0.1 CONFIG REQUIRED)` and link
`imvqml::imvqml`; the header is `<imvqml/imvimagejob.h>`.

## Encoded formats

Detection uses content rather than filenames. These readers run only in
`imvqml-decoder`; no encoded image is decoded by the application or ImageMagick.

| Extensions | Helper reader |
| --- | --- |
| `jpg`, `jpeg`, `png`, `gif`, `webp`, `tiff`, `tif` | libvips |
| `avif`, `heic`, `heif`, `jxl` | libvips with libheif or libjxl |
| `svg`, `svgz` | libvips with librsvg |
| `pnm`, `pgm`, `ppm`, `pbm` | libvips PNM (`sourceInfo.format: "ppm"`) |
| `jp2`, `j2k` | libvips with OpenJPEG (`sourceInfo.format: "jp2"`) |
| `exr`, `hdr` | libvips with OpenEXR and the Radiance loader |
| `bmp`, `ico` | Qt Gui and its ICO reader |
| `icns`, `tga` | Qt `QImageReader`, `qt6-imageformats` on Arch |
| `dds` | Qt `QImageReader`, `kimageformats` with DDS enabled |

Encoded images are limited to 256 MiB and 64 MiPixels per source frame. For
large local videos the helper streams the opened regular file instead of
copying it into memory. The encoded image cap does not apply to video; the
deadline, frame size and helper address-space limits still do. Large local
video frames bypass the persistent pixel cache.

## Display item

`ImvImage` (`<imvqml/imvimage.h>` in C++) is a Qt Quick item. Set its
width and height explicitly or with anchors. Its `source` is a path, local
`file:` URL, `data:` URL, or `image://provider/id`; `setBytes(bytes)` replaces
the URL with owned encoded bytes. Network URLs are rejected. Qt image
providers must return CPU-readable pixels; a GPU-only texture is not a byte
source. Provider sources are snapshotted when assigned. Synchronous providers
are called on the GUI thread for their native-size pixels; async provider
pixels are copied as soon as the response arrives. Resizing or changing fill
mode uses the snapshot, not a new provider request. No new codec runs in the
application for providers.

| Property | Behavior |
|---|---|
| `fillMode` | `ImvImage.Contain` (default), `Cover` (center crop), or `Stretch` |
| `mask`, `radius` | `NoMask` (default), `Circle` (inscribed), or `Rounded` with a radius in logical pixels |
| `tint` | Replaces the displayed RGB with this color and preserves the source alpha. `"transparent"` (default) disables tint. |
| `smooth` | Inherited Qt Quick texture filtering, true by default |
| `status` | `Idle` for no source, `Loading` awaiting size/decode/texture, `Ready` after scene-graph texture creation, or `Failed` |
| `sourceInfo` | `format`, oriented `width` and `height`, `frames`, `loop`; empty before an accepted result |
| `errorStage`, `errorCode`, `errorDetail` | The same failure vocabulary as jobs, plus `upload/texture-failed` |

Circle and rounded masks use antialiased pixel alpha in both the default and
software Qt Quick renderers. The circle is centered and inscribed in the item;
a rounded mask with radius zero is rectangular. Masking affects display only,
not the pixels returned or exported by a job.

Tint works the same way on both renderers. A tint with nonzero alpha supplies
RGB only; it does not change image opacity. Changing tint redraws the item
without decoding again or changing the pixels stored in the disk cache.
Tint alone is applied at decoded-frame size before the normal texture layout
and sampling. When a mask is active, both are applied in its physical-size
raster, bounded by the same output edge limit.

The request size is the item size times the window DPR, rounded up. Per-edge
resize jitter below the greater of 4 physical pixels and one eighth of the
previous requested edge reuses the prior decode. Old pixels can stay visible
during a resize but are dropped on source or fill-mode changes.

`animate` and `playing` default to true. Set `animate: false` for a first-frame
view; set `playing: false` to pause at the current frame. Hiding the item or
its window pauses playback and releases an idle streaming helper after 400 ms.
Showing it again resumes at the playhead if `playing` is still true. Finite
animations stop on the final frame. Multi-page images with no frame timing
stay on their first page. Image jobs always select the first frame.

Recognized local video containers (MP4/MOV, Matroska/WebM, AVI, and Ogg)
use a single captured frame, not playback. `ImvImage.videoPosition` and the
optional final `videoPosition` argument of `sample` and `render` select a
fraction from 0 (first frame) to 1 (near the end) when duration is known.
`sourceInfo.format` is `video` with oriented dimensions and one frame.
When compatible libmpv is unavailable, video yields
`inspect/unsupported-format` with `detail: "video-support-unavailable"`.

## Cache policy and thumbnails

`ImvCache` (`<imvqml/imvcache.h>` in C++) has process-wide policy:
`enabled` (default false), `directory` (an absolute local path; default
`$XDG_CACHE_HOME/imvqml` or `~/.cache/imvqml`), and `byteBudget` (512 MiB).
`clear()` removes owned entries from the configured store;
`invalidate(source)` targets entries for the current contents of a local file
or data URL. Both return a positive ticket immediately (`-3` on ticket
exhaustion, with no signal). `cleared(ticket, result)` is emitted once, queued
on the calling `ImvCache`, after the worker has settled eviction. Files still
leased by live items are protected. Neither operation clears pixels from
those items or touches freedesktop thumbnail exports.

The result contains `stage`, `code`, `detail`, and integer counts `removed`,
`kept` (leased entries), and `failed` (entries that could not be removed).
Success has `stage: "complete"` and empty `code` and `detail`. A busy directory
lock is retried; if it stays locked the result is `store/deadline`. On an
operation-level error before enumeration the entry counts are zero.
The counts describe this operation, not files written by later requests.
Store misses or corrupt entries fall back to the decoder. The store never
fetches a network source.

`exportThumbnail(source, size = 128)` is an explicit local-file export, even
if the private pixel store is off. `size` selects one of the freedesktop
buckets: 128, 256, 512, or 1024. A positive ticket yields
`thumbnailFinished(id, result)` with `stage`, `code`, `detail`, and on success
`destination`. The source is opened at call time. The output is an atomic PNG
with original URI and mtime; it is not written if the URI no longer names the
captured revision.

## Image jobs

`ImvImageJob` needs no display or window. Its methods accept local paths,
local `file:` URLs, and percent-encoded or base64 `data:` URLs. Jobs associated
with a QML engine also accept `image://provider/id`. The call owns the encoded
data URL or opened file descriptor, or snapshots the provider's pixels.
Async providers are requested during the call and snapshotted when their
response arrives. Files are copied off the GUI thread; rename or unlink does
not substitute a different file, but callers must not edit the open file in
place before it is copied. Jobs never fetch network URLs.
Uncompressed output edges must be 1 to 8192 pixels.

| Method | Successful result |
|---|---|
| `inspect(source)` | `sourceInfo` with `format`, oriented `width` and `height`, `frames`, `loop` (0 means infinite) |
| `sample(source, maxWidth, maxHeight, videoPosition = 0)` | `sourceInfo`, fitted `width` and `height`, and owned packed unpremultiplied RGBA8 `rgba` (QML `ArrayBuffer`) |
| `render(source, maxWidth, maxHeight, destination, format = "png", videoPosition = 0)` | `sourceInfo`, fitted `width` and `height`, and a published local file; `format` is `png` or `webp` (lossless) |

Sample and render select the first image frame and do not upscale. Render needs an
existing staging directory and an unused destination on a filesystem that
supports same-directory hard links. It never replaces a name. `cancel(id)`
returns false if the ticket is gone or atomic publication has begun; if it
returns true, the job cannot publish its output.

A positive ticket is returned immediately, including for queued work.
`finished(id, result)` is delivered once, queued, while the receiver exists.
The result always includes `stage`, `code`, `detail`; success is `complete` with
empty code and detail. A job can be cancelled by ticket while queued. `-2` is
rare: only a full shared queue returns busy, and no input is captured. Retry
only if the original input is still available. `-3` signals local ticket
exhaustion. Negative returns do not emit `finished`.

`ImvImage` remains `Loading` while waiting for admission. Visible items go
first; within each priority, pending requests go in arrival order. Source
replacement, a resize that changes the requested decode size, or item
destruction withdraws the old request. Display, jobs and thumbnail decodes
share the 128-request limit and the eight-second admission deadline. Jobs and
thumbnail decodes use background priority. Admission expiry is
`admission/deadline`; display and thumbnail decode overflow report the same
code. Helper execution has its own deadline. Video frame sources follow the
same rules. A render keeps its execution slot through encoding and
publication. Thumbnail writer overflow or expiry is `store/deadline`.

The process pool is private. Its capacity pressure is retryable admission
(`Busy`), not a terminal error. Process outcomes carry `stage: "helper"`
and one of `deadline`, `helper-crashed`, `protocol-error`, or `cancelled`.
Other codes: `admission` (`deadline`, `invalid-size`, `invalid-position`, `invalid-source`,
`invalid-destination`, `too-large`, `output-failed`), `open` (`unreadable`),
`inspect` (`unsupported-format`, `invalid-image`, `too-large`), `decode`
(`invalid-image`, `output-failed`) and `store` (`already-exists`, `deadline`,
`write-failed`). Treat diagnostics as plain text, not markup.
