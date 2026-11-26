# imvqml API

Import `ImvQml` from QML. For a custom prefix, add its `lib/qt6/qml`
directory to the QML import path (replace `lib` with CMake's install libdir).
C++ consumers use `find_package(imvqml 0.1 CONFIG REQUIRED)` and link
`imvqml::imvqml`; the header is `<imvqml/imvimagejob.h>`.

## Image jobs

`ImvImageJob` needs no display or window. Its methods accept local paths,
local `file:` URLs, and percent-encoded or base64 `data:` URLs. It never
fetches network URLs. Uncompressed output edges must be 1 to 8192 pixels.

| Method | Successful result |
|---|---|
| `inspect(source)` | `sourceInfo` with `format`, oriented `width` and `height`, `frames`, `loop` (0 means infinite) |
| `sample(source, maxWidth, maxHeight)` | `sourceInfo`, fitted `width` and `height`, and owned packed unpremultiplied RGBA8 `rgba` (QML `ArrayBuffer`) |
| `render(source, maxWidth, maxHeight, destination, format = "png")` | `sourceInfo`, fitted `width` and `height`, and a published local file; `format` is `png` or `webp` (lossless) |

Sample and render select the first frame and do not upscale. Render needs an
existing staging directory and an unused destination on a filesystem that
supports same-directory hard links. It never replaces a name. `cancel(id)`
returns false if the ticket is gone or atomic publication has begun; if it
returns true, the job cannot publish its output.

A positive ticket is local admission. `finished(id, result)` is delivered
once, queued, while the receiver exists. The result always includes `stage`,
`code`, `detail`; success is `complete` with empty code and detail. `-2`
means temporarily busy; retry with backoff. `-3` signals ticket exhaustion.
Negative admissions do not emit `finished`.

The process pool is private. Its capacity pressure is retryable admission
(`Busy`), not a terminal error. Process outcomes carry `stage: "helper"`
and one of `deadline`, `helper-crashed`, `protocol-error`, or `cancelled`.
Other codes: `admission` (`invalid-size`, `invalid-source`,
`invalid-destination`, `too-large`, `output-failed`), `open` (`unreadable`),
`inspect` (`unsupported-format`, `invalid-image`, `too-large`), `decode`
(`invalid-image`, `output-failed`) and `store` (`already-exists`,
`write-failed`). Treat diagnostics as plain text, not markup.
