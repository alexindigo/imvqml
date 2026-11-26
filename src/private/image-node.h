// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "image-pipeline.h"
#include <imvqml/imvimage.h>

namespace Imv {

QSGNode *updateImageNode(QSGNode *node, QQuickWindow *window, const Asset &asset, QSizeF size,
                         ImvImage::FillMode fill, ImvImage::Mask mask, qreal radius,
                         const QColor &tint, bool smooth);
quint64 textureUploads();

} // namespace Imv
