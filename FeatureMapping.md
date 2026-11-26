# Feature mapping

| Owner | Contract now | Proof |
|---|---|---|
| QML plugin / CMake package | Separate import and link, relocated prefix | Independent no-display consumers with original trees hidden |
| Private wire | Owned descriptors, typed length-framed request/reply, mock seam | `wire`: real helper, binary payload, descriptor echo, malformed frame and closure |
| Private pool | Bounded admission, hard deadline, exactly-once outcome, cancel, crash recovery and bounded native reuse | `pool`: native kill/hang/garbage, wire correlation, injected cancel/reply ordering, reuse and destruction |
| `ImvImageJob` | Headless inspect/sample/render, queued ticket result and no-clobber publication | `jobs`, `installed-sample`: metadata, actual output pixels, alpha, EXIF, error stages, cancellation and PNG/WebP publication |
| libvips helper | Explicit loader set, bounded source and geometry, validated sealed pixels | `jobs`: nine format fixtures, blocked PPM/FITS, malformed and oversized inputs, recovery |
| Persistent storage format | QOI, selected by timing before store implementation | [Measured comparison](tests/store-format.md), `imv-store-benchmark` |
| `ImvImage` / private pipeline | Physical-size requests, owned sources, coalescing, cancellable interests and stale-delivery guards | `pipeline`: admitted cancellation and surviving shared interest; `items`: same-size coalescing, resize threshold, superseded source and provider/byte pixels |
| Render-thread texture cache | Per-graph leases and byte-budgeted LRU | `items`: niri window pixels with one decode and one texture across two items, then reuse without a decode or texture creation |
| Animation playback | Sealed small batches, dedicated streamed page helpers, bounded lookahead, delays, visibility and finite loops | `pipeline`: GIF/WebP delay metadata and pixels, large GIF ring, still availability and helper retirement/resume; `items`: two loops and paused/hidden window playback |
