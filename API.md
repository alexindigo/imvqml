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

Local files are opened and checked at source assignment, even if the item has
no window or size yet. A refusal sets `Failed` and the error properties before
the setter returns, without entering `Loading`. Successful assignments keep
the opened revision for decoding and resizing. To retry a refused source,
clear `source` and assign it again. File contents are still read off the GUI
thread.

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
If a native video operation fails after loading the runtime, `detail` can add
the failing call or event, libmpv's error, the calling thread's `errno` when
available, and the helper process's lifetime peak virtual address space
(`VmPeak`, when available). It can also include up to five recent warning or
error lines from that capture, truncated individually. Asynchronous events
do not report another thread's `errno`. This diagnostic text is for humans;
use `stage` and `code` for programmatic handling. No diagnostics are written
to the terminal or included in a successful result.

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
| `retainOriginal(source, destinationDirectory)` | Absolute local `path`, `size` in bytes, lowercase hex `sha256`, and detected `format` (the same format names as `inspect`) |

Sample and render select the first image frame and do not upscale. Render needs an
existing staging directory and an unused destination on a filesystem that
supports same-directory hard links. It never replaces a name. `cancel(id)`
returns false if the ticket is gone or atomic publication has begun; if it
returns true, the job cannot publish its output.

`retainOriginal` preserves the exact encoded bytes of file and `data:`
sources, including metadata and all animation frames. At call time it opens
local inputs with `O_NONBLOCK` and `O_CLOEXEC`, rejects non-regular inputs,
and checks the 256 MiB source limit on the descriptor (no video exception).
It reads no file contents on the GUI thread. You can unlink the source or
replace its directory entry immediately after the call, but must not modify
the opened file in place. The worker snapshots from that descriptor, the
decoder inspects the snapshot, and the worker publishes those same bytes.
Inspection errors prevent publication. Providers expose pixels, not encoded
bytes; their native snapshot is retained as a lossless PNG (`format: "png"`).

Supply an existing, writable local directory that supports hard links. Keep
its path stable while the job runs. The job generates its own opaque file
name, never overwrites a file, and does not manage the result after success.

A positive ticket is returned for accepted work, including queued work.
`finished(id, result)` is delivered once, queued, while the receiver exists.
The result always includes `stage`, `code`, `detail`; success is `complete` with
empty code and detail. A job can be cancelled by ticket while queued.

A call-time refusal returns a negative result immediately, without allocating
a ticket or emitting `finished`. Read the job's `lastErrorStage`,
`lastErrorCode`, and `lastErrorDetail` immediately after the call. They describe
the most recent submission, are cleared by an accepted submission, and are not
binding notifications or updated by an asynchronous completion.

| Result / `ImvImageJob` enum | Meaning |
|---|---|
| `-1` / `InvalidRequest` | Invalid arguments, unsupported source URL, missing provider, or another synchronous failure not listed below. |
| `-2` / `Busy` | Shared queue is full (`admission/busy`); no input is captured. Retry only if the input is still available. |
| `-3` / `TicketExhausted` | This job object has no more ticket IDs (`admission/ticket-exhausted`). |
| `-4` / `UnreadableSource` | Source cannot be opened or is not a readable regular file (`open/unreadable`), or a synchronous provider did not supply readable pixels. |
| `-5` / `SourceTooLarge` | Capture already established a size refusal, for example `retainOriginal`'s `fstat` size limit (`admission/too-large`). |

For `inspect`, `sample`, and `render`, a local file over 256 MiB may be a
streamable video. The helper determines its format; any resulting size refusal
is asynchronous. Invalid encoded content, errors reading the opened revision,
and failures reported later by an asynchronous provider also use `finished`.

`ImvImage` remains `Loading` while waiting for admission. Visible items go
first; within each priority, pending requests go in arrival order. Source
replacement, a resize that changes the requested decode size, or item
destruction withdraws the old request. Display, jobs and thumbnail decodes
share the 128-request limit and the eight-second admission deadline. Jobs and
thumbnail decodes use background priority. Admission expiry is
`admission/deadline`; display and thumbnail decode overflow report the same
code. Helper execution has its own deadline. Video frame sources follow the
same rules. Render and retention jobs keep their execution slot through
publication. Thumbnail writer overflow or expiry is `store/deadline`.

The process pool is private. Its capacity pressure is retryable admission
(`Busy`), not a terminal error. Process outcomes carry `stage: "helper"`
and one of `deadline`, `helper-crashed`, `protocol-error`, or `cancelled`.
Other codes: `admission` (`deadline`, `invalid-size`, `invalid-position`, `invalid-source`,
`invalid-destination`, `too-large`, `output-failed`), `open` (`unreadable`),
`inspect` (`unsupported-format`, `invalid-image`, `too-large`), `decode`
(`invalid-image`, `output-failed`) and `store` (`already-exists`, `deadline`,
`write-failed`). Treat diagnostics as plain text, not markup.
