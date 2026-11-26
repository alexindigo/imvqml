// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include <QByteArrayView>
#include <QtEndian>

namespace Imv {
// This is container admission, not decoding. Read no more than a bounded
// prefix and never send known image BMFF brands to the video lane.
inline constexpr int VideoSignatureBytes = 128;
inline const char *videoDemuxer(QByteArrayView head) {
    if (head.size() < 12)
        return nullptr;
    if (head.startsWith(QByteArrayView("\x1a\x45\xdf\xa3", 4)))
        return "matroska";
    const auto box = head.sliced(4, 4);
    if (box == "ftyp") {
        const auto end = qMin<qsizetype>(head.size(), qFromBigEndian<quint32>(head.data()));
        for (qsizetype i = 8; i + 4 <= end; i += 4) {
            if (i == 12) // minor version, not a brand
                continue;
            const auto brand = head.sliced(i, 4);
            if (brand == "avif" || brand == "avis" || brand == "mif1" || brand == "msf1" ||
                brand == "miaf" || brand.startsWith("hei") || brand.startsWith("hev"))
                return nullptr;
        }
        return "mov";
    }
    if (box == "moov" || box == "mdat" || box == "wide")
        return "mov";
    if (head.startsWith("RIFF") && head.sliced(8, 4) == "AVI ")
        return "avi";
    if (head.startsWith("OggS"))
        return "ogg";
    return nullptr;
}
} // namespace Imv
