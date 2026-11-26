# imvqml API

Import `ImvQml` from QML. For a custom prefix, add its `lib/qt6/qml`
directory to the QML import path (replace `lib` with CMake's install libdir).
C++ consumers use `find_package(imvqml 0.1 CONFIG REQUIRED)` and link
`imvqml::imvqml`; the header is `<imvqml/imvimagejob.h>`.

`ImvImageJob` is a non-visual QObject and needs no display or window.
In this scaffold, `sample(source, maxWidth, maxHeight)` admits a nonempty
URL and positive dimensions, returning a positive ticket, or rejects
immediately with `-1`. Each ticket gets one queued `finished(id, result)`
while the receiver exists. The scaffold reports `stage: "decode"`,
`code: "not-implemented"` and a diagnostic `detail`; it does not decode.
