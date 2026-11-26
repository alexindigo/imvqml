// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "wire.h"
#include <QSize>
#include <QString>

namespace Imv {

inline constexpr qint64 MaxSourceBytes = 256ll * 1024 * 1024;
inline constexpr qint64 MaxSourcePixels = 64ll * 1024 * 1024;
inline constexpr int MaxEdge = 8192;
inline constexpr int StillDeadlineMs = 8000;
inline constexpr int AnimationDeadlineMs = 30000;
inline constexpr qint64 AnimationBudget = 64ll * 1024 * 1024;
inline constexpr int MemfdSeals = 0x0f; // SEAL, SHRINK, GROW, WRITE

struct DecodeSpec {
    enum Mode : int { Inspect, Pixels, Animation, FrameAt } mode = Inspect;
    QSize bounds{0, 0};
    enum Scale : int { Contain, Cover, Stretch } scale = Contain;
    int page = 0;
    double position = 0;
    bool valid() const;
};

struct SourceInfo {
    QString format;
    QSize size;
    int frames = 1;
    int loop = 1; // zero = infinite
    bool valid() const;
};

enum class DecodeCode {
    Ok,
    Unreadable,
    Unsupported,
    InvalidHeader,
    SourceLimit,
    PixelLimit,
    Decode,
    Output
};

struct DecodeReport {
    DecodeCode code = DecodeCode::Ok;
    SourceInfo source;
    QSize output{0, 0};
    QString detail;
    enum Layout : int { Single, Batch, Stream } layout = Single;
    int delay = 0; // milliseconds for the single frame or first streamed frame
    qint64 outputBytes() const;
};

QByteArray encode(const DecodeSpec &spec);
QByteArray encode(const DecodeReport &report);
bool decode(QByteArrayView bytes, DecodeSpec &spec);
bool decode(QByteArrayView bytes, DecodeReport &report);
qint64 rgbaBytes(QSize size);
Fd newMemfd(const char *name);
bool writeAll(int fd, const char *data, qint64 size, qint64 offset = 0);

} // namespace Imv
