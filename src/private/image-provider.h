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

// Capture at native size on the GUI thread. Synchronous providers deliver
// inline; async responses remain owned until finished() even if cancelled.
std::function<void()> snapshotProvider(QQmlEngine *engine, const QUrl &url, QObject *receiver,
                                       std::function<void(ProviderSnapshot)> done);
Decoded rasterizeProvider(const ProviderSnapshot &snapshot, DecodeSpec spec);

} // namespace Imv
