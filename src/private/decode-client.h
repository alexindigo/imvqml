// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "decode-wire.h"
#include "pool.h"
#include <QImage>
#include <QUrl>
#include <QVariantMap>

namespace Imv {
struct OpenedSource {
    Fd fd;
    QByteArray identity;
    Failure error;
};
struct Decoded {
    SourceInfo source;
    QImage pixels;
    Failure error;
};

QString localPath(const QUrl &url);
OpenedSource openSource(const QUrl &url);
OpenedSource openSource(const QByteArray &bytes);
Pool &decoderPool();
Pool::Ticket submitDecode(Pool &pool, Fd input, DecodeSpec spec, std::function<void(Decoded)> done);
QVariantMap asMap(const SourceInfo &source);
QVariantMap asMap(const Failure &failure);
} // namespace Imv
