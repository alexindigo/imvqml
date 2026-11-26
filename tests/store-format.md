# Persistent entry format measurement

Selected **QOI**. Timing came first, then size. Compared zstd level 3 on
packed RGBA8, the QOI reference codec, and lossless WebP. All decoded to
the same byte-exact RGBA8 before the timed trials. Tested on `imvqml-dev`
on ark, with the niri Wayland session and virgl (Intel Arc Pro B50).

Each row reports 31 trials per lane after 3 untimed iterations. The
benchmark rotates codec order and times open/read -> decode -> actual
OpenGL texture upload and `glFinish`. For cold trials it requests per-file
guest page-cache eviction with `POSIX_FADV_DONTNEED` (not a claim about the
host/disk caches). File size is compressed payload only, before store metadata.

| Fixture | Codec | Bytes | Warm median / p90 (ms) | Cold median / p90 (ms) |
|---|---|---:|---:|---:|
| Alpha icon 256x256 | zstd | 18437 | 0.396 / 0.911 | 0.485 / 1.362 |
| Alpha icon 256x256 | QOI | 47425 | 0.419 / 1.326 | 0.633 / 1.203 |
| Alpha icon 256x256 | WebP | 3360 | 0.564 / 1.587 | 0.963 / 1.999 |
| Gradient 640x360 | zstd | 144766 | 2.357 / 3.117 | 2.594 / 3.287 |
| Gradient 640x360 | QOI | 187611 | 1.633 / 2.981 | 1.479 / 2.360 |
| Gradient 640x360 | WebP | 4826 | 1.836 / 2.880 | 2.036 / 3.169 |
| Detailed 1920x1080 | zstd | 5367415 | 13.379 / 14.591 | 15.813 / 17.259 |
| Detailed 1920x1080 | QOI | 4084561 | 10.493 / 12.060 | 11.888 / 13.540 |
| Detailed 1920x1080 | WebP | 2290008 | 34.570 / 36.733 | 35.013 / 37.089 |

Sum of warm/cold medians: QOI 26.545 ms, zstd 35.024 ms, WebP 74.983 ms.
QOI won both lane totals and four of six individual medians; zstd won
the small alpha icon. Total payload was 4,319,597 bytes for QOI, versus
5,530,618 for zstd and 2,298,194 for WebP. WebP was smallest but not fastest.
These results select the format, not a guarantee about user workloads.
Cache reads in unit 6 must validate bounded geometry and encoded lengths;
the benchmark's reference codec only reads known well-formed samples.
