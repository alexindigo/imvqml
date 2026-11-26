// SPDX-License-Identifier: LGPL-3.0-only
#include "decode-wire.h"

#include <QtEndian>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace Imv {
namespace {
constexpr int ReportHeader = 32;
constexpr int MaxDetail = 1024;
} // namespace

bool DecodeSpec::valid() const {
    if (mode == Inspect)
        return bounds == QSize(0, 0);
    return mode == Pixels && rgbaBytes(bounds) > 0;
}

bool SourceInfo::valid() const {
    return !format.isEmpty() && format.size() <= 16 && size.width() > 0 && size.height() > 0 &&
           qint64(size.width()) * size.height() <= MaxSourcePixels && frames >= 1 && loop >= 0;
}

qint64 rgbaBytes(QSize size) {
    if (size.width() < 1 || size.height() < 1 || size.width() > MaxEdge || size.height() > MaxEdge)
        return 0;
    return qint64(size.width()) * size.height() * 4;
}

QByteArray encode(const DecodeSpec &spec) {
    QByteArray output(12, Qt::Uninitialized);
    qToLittleEndian(qint32(spec.mode), output.data());
    qToLittleEndian(qint32(spec.bounds.width()), output.data() + 4);
    qToLittleEndian(qint32(spec.bounds.height()), output.data() + 8);
    return output;
}

bool decode(QByteArrayView bytes, DecodeSpec &spec) {
    if (bytes.size() != 12)
        return false;
    auto *p = bytes.data();
    spec = {DecodeSpec::Mode(qFromLittleEndian<qint32>(p)),
            QSize(qFromLittleEndian<qint32>(p + 4), qFromLittleEndian<qint32>(p + 8))};
    return spec.valid();
}

QByteArray encode(const DecodeReport &report) {
    const auto format = report.source.format.toLatin1();
    const qint32 fields[]{qint32(report.code),         report.source.size.width(),
                          report.source.size.height(), report.source.frames,
                          report.source.loop,          report.output.width(),
                          report.output.height(),      qint32(format.size())};
    QByteArray output(ReportHeader, Qt::Uninitialized);
    qToLittleEndian<qint32>(fields, std::size(fields), output.data());
    output.append(format);
    output.append(report.detail.toUtf8().left(MaxDetail));
    return output;
}

bool decode(QByteArrayView bytes, DecodeReport &report) {
    if (bytes.size() < ReportHeader || bytes.size() > ReportHeader + 16 + MaxDetail)
        return false;
    qint32 fields[ReportHeader / 4];
    qFromLittleEndian<qint32>(bytes.data(), std::size(fields), fields);
    const auto [code, sw, sh, frames, loop, ow, oh, nameSize] = fields;
    if (code < 0 || code > int(DecodeCode::Output) || nameSize < 0 || nameSize > 16 ||
        ReportHeader + nameSize > bytes.size() ||
        bytes.size() - ReportHeader - nameSize > MaxDetail)
        return false;
    report = {
        DecodeCode(code),
        {QString::fromLatin1(bytes.sliced(ReportHeader, nameSize)), QSize(sw, sh), frames, loop},
        QSize(ow, oh),
        QString::fromUtf8(bytes.sliced(ReportHeader + nameSize))};
    if (report.code != DecodeCode::Ok)
        return !report.detail.isEmpty() && report.source.format.isEmpty() &&
               report.output == QSize(0, 0);
    return report.detail.isEmpty() && report.source.valid() &&
           (report.output == QSize(0, 0) || rgbaBytes(report.output) > 0);
}

Fd newMemfd(const char *name) {
    static_assert(MemfdSeals == (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE));
    return Fd(::memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING));
}

bool writeAll(int fd, const char *data, qint64 size) {
    qint64 position = 0;
    while (position < size) {
        const auto wrote = ::pwrite(fd, data + position, size - position, position);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            return false;
        position += wrote;
    }
    return true;
}

} // namespace Imv
