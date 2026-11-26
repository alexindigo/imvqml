// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "decode-client.h"

namespace Imv {
QByteArray encodeEntry(const Decoded &decoded, qint64 budget);
std::optional<Decoded> decodeEntry(QByteArrayView entry, DecodeSpec spec);
inline constexpr qint64 MaxEntryBytes = qint64(MaxEdge) * MaxEdge * 5 + 65536;
} // namespace Imv
