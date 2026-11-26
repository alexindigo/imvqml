# Feature mapping

| Owner | Contract now | Proof |
|---|---|---|
| QML plugin / CMake package | Separate import and link, relocated prefix | Independent no-display consumers with original trees hidden |
| Private wire | Owned descriptors, typed length-framed request/reply, mock seam | `wire`: real helper, binary payload, descriptor echo, malformed frame and closure |
| Private pool | Bounded admission, hard deadline, exactly-once outcome, cancel, crash recovery and bounded native reuse | `pool`: native kill/hang/garbage, wire correlation, injected cancel/reply ordering, reuse and destruction |
| `ImvImageJob` | Headless inspect/sample/render, queued ticket result and no-clobber publication | `jobs`, `installed-sample`: metadata, actual output pixels, alpha, EXIF, error stages, cancellation and PNG/WebP publication |
| libvips helper | Explicit loader set, bounded source and geometry, validated sealed pixels | `jobs`: nine format fixtures, blocked PPM/FITS, malformed and oversized inputs, recovery |
| Persistent storage format | QOI, selected by timing before store implementation | [Measured comparison](tests/store-format.md), `imv-store-benchmark` |
