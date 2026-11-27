// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "decode-client.h"

class QQmlEngine;

namespace Imv {

struct ProviderSnapshot {
    QSize original;
    QImage pixels;
    Failure error;
};

// Synchronous providers return their snapshot or failure. Pending providers
// return a cancellation handle and deliver their result via done.
using ProviderCapture = std::variant<ProviderSnapshot, std::function<void()>>;
ProviderCapture snapshotProvider(QQmlEngine *engine, const QUrl &url, QObject *receiver,
                                 std::function<void(ProviderSnapshot)> done);
Decoded rasterizeProvider(const ProviderSnapshot &snapshot, DecodeSpec spec);

} // namespace Imv
