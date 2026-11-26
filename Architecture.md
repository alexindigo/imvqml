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
and exits on EOF. The probe helper echoes opaque binary and descriptors;
there is no codec in this unit.

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
