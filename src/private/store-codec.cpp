// SPDX-License-Identifier: LGPL-3.0-only
#include "store-codec.h"

#include <QCryptographicHash>
#include <QtEndian>

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "qoi.h"

namespace Imv {
namespace {
void put(QByteArray &data, quint32 field) {
    field = qToLittleEndian(field);
    data.append(reinterpret_cast<const char *>(&field), 4);
}

bool take(QByteArrayView &data, quint32 &field) {
    if (data.size() < 4)
        return false;
    field = qFromLittleEndian<quint32>(data.data());
    data = data.sliced(4);
    return true;
}

bool validQoi(QByteArrayView data, QSize size) {
    constexpr QByteArrayView End("\0\0\0\0\0\0\0\1", 8);
    if (data.size() < 23 || !data.startsWith("qoif") || !data.endsWith(End) ||
        qFromBigEndian<quint32>(data.data() + 4) != quint32(size.width()) ||
        qFromBigEndian<quint32>(data.data() + 8) != quint32(size.height()) || data[12] != 4 ||
        data[13] != 0)
        return false;
    qsizetype p = 14;
    const auto end = data.size() - End.size();
    qint64 pixels = rgbaBytes(size) / 4;
    while (p < end && pixels > 0) {
        const uchar operation = data[p++];
        if (operation == 0xfe) {
            p += 3;
        } else if (operation == 0xff) {
            p += 4;
        } else if ((operation & 0xc0) == 0x80) {
            ++p;
        } else if ((operation & 0xc0) == 0xc0) {
            pixels -= operation & 0x3f;
        }
        --pixels;
    }
    return p == end && pixels == 0;
}

QImage unpack(QByteArrayView data, QSize size) {
    if (!validQoi(data, size))
        return {};
    qoi_desc description{};
    auto *buffer = static_cast<uchar *>(qoi_decode(data.data(), int(data.size()), &description, 4));
    if (!buffer)
        return {};
    return QImage(
        buffer, size.width(), size.height(), size.width() * 4, QImage::Format_RGBA8888,
        [](void *data) { free(data); }, buffer);
}
} // namespace

QByteArray encodeEntry(const Decoded &value, qint64 budget) {
    DecodeReport report;
    if (value.error) {
        if (value.error.stage != "inspect" || value.error.code != "unsupported-format" ||
            value.error.detail == "video-support-unavailable")
            return {};
        report.code = DecodeCode::Unsupported;
        report.detail = value.error.detail;
    } else {
        report.source = value.source;
        report.output = value.pixels.size();
        report.layout = value.stream            ? DecodeReport::Stream
                        : value.animation.count ? DecodeReport::Batch
                                                : DecodeReport::Single;
        report.delay = value.delay;
        if (value.pixels.isNull())
            return {};
    }
    QByteArray out = "imvqoi-1";
    const auto metadata = encode(report);
    put(out, metadata.size());
    out.append(metadata);
    if (!value.error) {
        const int count = value.animation.count ? value.animation.count : 1;
        for (int i = 0; i < count; ++i) {
            const auto image = value.animation.count ? value.animation.frame(i) : value.pixels;
            const qoi_desc desc{unsigned(image.width()), unsigned(image.height()), 4, 0};
            int bytes = 0;
            const auto packed = std::unique_ptr<void, decltype(&free)>(
                qoi_encode(image.constBits(), &desc, &bytes), free);
            const int overhead = 32 + 4 + (count > 1 ? 4 : 0);
            if (!packed || out.size() + bytes + overhead > qMin(budget, MaxEntryBytes))
                return {};
            if (count > 1)
                put(out, value.animation.delay(i));
            put(out, bytes);
            out.append(static_cast<const char *>(packed.get()), bytes);
        }
    }
    out.append(QCryptographicHash::hash(out, QCryptographicHash::Sha256));
    return out.size() <= budget ? out : QByteArray{};
}

std::optional<Decoded> decodeEntry(QByteArrayView data, DecodeSpec spec) {
    if (data.size() < 84 || data.size() > MaxEntryBytes || !data.startsWith("imvqoi-1") ||
        QCryptographicHash::hash(data.chopped(32), QCryptographicHash::Sha256) != data.last(32))
        return {};
    data = data.sliced(8, data.size() - 40);
    quint32 bytes = 0;
    DecodeReport report;
    if (!take(data, bytes) || data.size() < bytes || !decode(data.first(bytes), report))
        return {};
    data = data.sliced(bytes);
    if (report.code == DecodeCode::Unsupported && data.empty())
        return Decoded{{}, {}, {"inspect", "unsupported-format", report.detail}};
    if (report.code != DecodeCode::Ok || !report.outputBytes() ||
        report.output.width() > spec.bounds.width() ||
        report.output.height() > spec.bounds.height() ||
        (spec.mode != DecodeSpec::Animation && report.layout != DecodeReport::Single))
        return {};
    Decoded value{report.source, {}, {}, {}, report.layout == DecodeReport::Stream, report.delay};
    if (report.layout == DecodeReport::Batch) {
        const auto frameBytes = rgbaBytes(report.output);
        auto memory = std::shared_ptr<uchar[]>(new uchar[report.outputBytes()]);
        for (int i = 0; i < report.source.frames; ++i) {
            quint32 delay = 0;
            if (!take(data, delay) || delay == 0 || delay > INT_MAX || !take(data, bytes) ||
                data.size() < bytes)
                return {};
            const auto frame = unpack(data.first(bytes), report.output);
            if (frame.isNull())
                return {};
            auto *dst = memory.get() + i * (frameBytes + 4);
            qToLittleEndian(delay, dst);
            memcpy(dst + 4, frame.constBits(), frameBytes);
            data = data.sliced(bytes);
        }
        value.animation = {std::shared_ptr<const uchar>(memory, memory.get()), report.output,
                           report.source.frames};
        value.pixels = value.animation.frame(0);
    } else {
        if (!take(data, bytes) || data.size() != bytes)
            return {};
        value.pixels = unpack(data, report.output);
        if (value.pixels.isNull())
            return {};
        data = {};
    }
    if (!data.empty())
        return {};
    return value;
}
} // namespace Imv
