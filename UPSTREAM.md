# Upstream sources and dependencies

- mpvqml v0.3.0 (`56a1fc63686e6c87b46cd6d75f79622b81cca757`), LGPL-3.0-only:
  discovery, producer and installed-consumer architecture reference. The
  license files are copied from that tag. No capture code is ported yet.
- Qt 6.8 or newer: QML registration and descriptor-preserving process
  setup (`QProcess::setChildProcessModifier`, `UnixProcessParameters`).
- No libvips, loaders or libmpv are linked by this scaffold.
