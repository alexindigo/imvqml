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
One precise timer per admitted slot enforces the wall-clock budget; a reply
drained after the deadline cannot revive an expired ticket. Cancel, timeout,
garbage and crash all converge on the same terminal ledger. Capacity remains
occupied through child reaping if a child must stop, and release is delivered
after the terminal callback. Pool destruction closes sockets and requests child
kill, but a detached retirement receiver owns the process until Qt reaps it.

## Decode and publication

Only the decoder helper runs libvips. At startup it blocks libvips-marked
untrusted operations and all foreign loaders. One table in `vips-decode.cpp`
then enables only the `_source` loaders it uses: PNG, JPEG, GIF (nsgif),
WebP, TIFF, SVG, HEIF/AVIF, JPEG XL. It accepts no media paths; a nameless,
seekable source reads only the passed descriptor, capped at the admitted
length (256 MiB maximum). Unknown and non-allow-listed formats converge on
`inspect/unsupported-format`. This is crash containment, not a general OS
sandbox. Body/schema validation remains separate from the loader policy.

Read header dimensions and verify the source's 64 MiPixel limit before
thumbnailing. libvips handles shrink-on-load when the loader supports it,
applies orientation and emits fitted sRGB RGBA8. The client passes an empty
sealable output descriptor, and the helper seals it against all writes and
size changes after producing pixels. A typed reply is accepted only when
its geometry and the sealed descriptor size agree. A read-only mapping
backs the owned frame value; the caller does not decode a PNG or WebP.
The pool's still deadline is 8 seconds and helper address space is 1 GiB.

`ImvImageJob` maps client IDs to the pool ledger and separates inspect,
sample and render results. Export passes already-decoded pixels to a Qt
worker for encoding. It stages in a descriptor-held directory and a
single cancellation/publication gate precedes `linkat` no-clobber commit.
