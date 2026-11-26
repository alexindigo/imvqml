# Architecture

The backing library owns application state; the separate QML plugin
registers it with the engine. The decoder is an installed executable,
resolved by anchoring the relative libexec path at the backing library's
mapped file. It is not resolved through the working directory or `PATH`.

The private `Transport` seam separates typed frame ownership from Linux
socket I/O. `AF_UNIX` / `SOCK_SEQPACKET` preserves message boundaries.
Protocol v1 uses a 24-byte little-endian header: four magic bytes, 16-bit
version, 16-bit kind, 64-bit nonzero serial, 32-bit body length, 16-bit
descriptor count and 16 reserved zero bits. Body length is capped at
32 KiB and there are at most two `SCM_RIGHTS` descriptors per frame.
Received descriptors are close-on-exec and have move-only owners even when a
packet is rejected. The helper inherits exactly one protocol endpoint (fd 3)
and exits on EOF. The wire probe echoes opaque binary and descriptors.

The private GUI-affine pool owns two reusable children by default, with at
most 32 completed requests per child. It exposes a factory of injectable
children implementing the same transport and retirement signals. Every local
positive ticket has an independent wire serial and one terminal owner, consumed
before queuing delivery. Nonblocking notifiers are armed only for relevant I/O.
One precise timer per admitted slot tracks the wall-clock budget; a reply
drained after the deadline cannot revive an expired ticket. A monotonic kernel
timer in the helper kills a wedged backend independently of GUI events. Cancel, timeout,
garbage and crash all converge on the same terminal ledger. Capacity remains
occupied through child reaping if a child must stop, and release is delivered
after the terminal callback. Pool destruction closes sockets and requests child
kill, but a detached retirement receiver owns the process until Qt reaps it.

## Decode and publication

Only the decoder helper runs libvips. At startup it blocks libvips-marked
untrusted operations and all foreign loaders. One table in `vips-decode.cpp`
then enables only the `_source` loaders it uses: PNG, JPEG, GIF (nsgif),
WebP, TIFF, SVG, HEIF/AVIF, JPEG XL. It accepts no media paths. Both backends
receive only a nameless, seekable, sealed snapshot at most 256 MiB long.
The helper validates this contract before selecting a backend. Unknown and non-allow-listed formats converge on
`inspect/unsupported-format`. This is crash containment, not a general OS
sandbox. Body/schema validation remains separate from the loader policy.

Read header dimensions and verify the source's 64 MiPixel limit before
thumbnailing. libvips handles shrink-on-load when the loader supports it,
applies orientation and emits fitted sRGB RGBA8. For pixel requests the client
passes a sparse output descriptor sized to the request's maximum and sealed
against growth. The helper shrinks it to the actual output size and applies
the remaining seals before a successful reply. Inspect has no output descriptor.
A pixel reply is accepted only when its geometry, all seals and the exact
descriptor size agree. A read-only mapping
backs the owned frame value; the caller does not decode a PNG or WebP.
The pool's still deadline is 8 seconds and helper address space is 1 GiB.

`ImvImageJob` maps client IDs to the pool ledger and separates inspect,
sample and render results. Export passes already-decoded pixels to a Qt
worker for encoding. Two job permits cover provider or helper work through
encoding and delivery. A separate bounded source queue materializes encoded
inputs outside the GUI thread; pending store lookups borrow their caller's input.
Export stages in a descriptor-held directory and a
single cancellation/publication gate precedes `linkat` no-clobber commit.

## Display

`ImvImage` adapts sources and snapshots physical bounds and fill intent. The
GUI-affine pipeline keys requests by source revision (file stat including
ctime, or digest of immutable owned bytes) and the encoded decode spec. It
coalesces waiting and submitted decodes, queues behind pool capacity, and
revokes delivery interest independently of helper retirement. A generation
guards decode and texture outcomes at the item. The pipeline's result index
is weak: it owns neither idle mappings nor scene-graph resources.

Qt image providers are a separate source boundary: items and image jobs take
ownership of native-size CPU pixels on the GUI thread at assignment or job
admission, or when an async response arrives. Fitting and rendering read only
that snapshot, so geometry changes cannot re-request transient provider pixels.

Each window's scene graph owns a render-thread texture LRU. Scene-graph nodes
hold texture leases; texture entries hold the decoded asset for reuse. One
texture is shared across matching presentations. Masks are antialiased alpha
variants generated at the texture boundary, so they work with software and
GPU renderers without requiring separate decodes or disk entries. The texture
key includes the active mask and tint. Only mask variants bake in the radius,
fill, physical output geometry, and sampling mode. Without a mask, tint is a
decoded-frame-sized RGB pass and the scene graph applies layout and filtering
as for the untinted texture. Tint never changes the raw decoded or persistently
cached pixels. Eviction is
least-recently-used among unleased entries, trimmed at cache pressure and on
release; active leases can temporarily
exceed the 128 MiB target. Scene-graph invalidation drops the cache on the
render thread.

## Animation

The pipeline classifies timed sources before pixel submission: still decode
uses the 8 s deadline and the animation decode uses 30 s. At most 64 MiB of
packed display-size frames and delays are returned as one sealed mapping.
Frame views share the mapping rather than copying the image stack. If the
budget would be exceeded, the initial result carries only the first frame
and the caller retains the opened input descriptor for playback.

Each active stream has a separate one-slot playback pool. One request reads
one composited source page using libvips' page selection. A two-frame lookahead
ring is consumer-driven and streamed textures are not retained in the LRU.
The decoder reparses the encoded source as needed; decoded pixels do not
grow with the frame count. Timers own delay, loop and pause state; the item
passes visibility and play intent. Paused streams retire their helper and
lookahead after a grace period; they do not take still-pool capacity.

## Persistent store

One serial store worker owns content hashing, QOI envelope validation,
publication and eviction. Keys pair SHA-256 of the same immutable snapshot
used to classify and decode the request with
decode parameters and the imvqml build version, libvips version and loader
implementation fingerprint. The key also includes the system SDK libvips
file identity so upgrading that library breaks reuse across installs. An
entry may hold a still, first streamed frame, small frame batch, or stable
unsupported-format outcome. A cache epoch separates policy changes and
explicit invalidation from in-flight work. Files are atomically replaced
under a short directory lock; LRU eviction skips live asset leases.

Freedesktop thumbnails are explicit first-frame jobs. The store worker
encodes trusted output to a PNG in the requested standard bucket, including
thumbnail URI and modification metadata. The private cache and the shared
thumbnail directories have separate ownership and deletion paths.

## Optional video

The helper probes a bounded container signature only after libvips rejects
the source. It loads libmpv dynamically and selects a restricted demuxer on
the already-open descriptor. The transaction independently correlates the
native command reply and the load or seek event followed by playback restart.
The software renderer sees unrotated input; the helper uses source metadata
to orient pixels before publishing the same sealed RGBA result as stills.
The application does not link libmpv or mpvqml. The helper-pool deadline and
crash boundary also apply to native initialization and teardown.

## Installation boundary

The QML module, backing library, CMake target and decoder install together.
Independent probes rebuild against the moved prefix with both original trees
overlaid by empty directories. They verify relocated ELF mappings and pixels
from headless C++ and QML jobs and a Wayland QML item.
