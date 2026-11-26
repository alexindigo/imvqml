// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "image-pipeline.h"

class QQmlEngine;

namespace Imv {

// One caller's delivery interest. A cancelled response remains owned until
// finished(), per QQuickImageResponse's contract.
std::function<void()> requestProvider(QQmlEngine *engine, const QUrl &url, DecodeSpec spec,
                                      QObject *receiver, ImagePipeline::Done done);

} // namespace Imv
