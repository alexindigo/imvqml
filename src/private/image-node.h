// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "image-pipeline.h"
#include <imvqml/imvimage.h>

namespace Imv {

struct TextureOutcome {
    QSGNode *node;
    bool uploaded;
};
TextureOutcome updateImageNode(QSGNode *node, QQuickWindow *window, const Asset &asset, QSizeF size,
                               ImvImage::FillMode fill, ImvImage::Mask mask, qreal radius,
                               const QColor &tint, bool smooth, quint64 sourceEpoch);
quint64 textureUploads();
void failNextTextureUpload(QQuickWindow *window);

} // namespace Imv
