# Feature mapping

| Owner | Contract now | Proof |
|---|---|---|
| QML plugin / CMake package | Separate import and link, relocated prefix | Independent no-display consumers with original trees hidden |
| Private wire | Owned descriptors, typed length-framed request/reply, mock seam | `wire`: real helper, binary payload, descriptor echo, malformed frame and closure |
| Private pool | Bounded admission, hard deadline, exactly-once outcome, cancel, crash recovery and bounded native reuse | `pool`: native kill/hang/garbage, wire correlation, injected cancel/reply ordering, reuse and destruction |
| `ImvImageJob` | Headless type and queued ticket result; no decode yet | `installed-prefix`; opt-in `installed-pixels-red` checks real expected RGBA bytes and is red until decoding lands |
