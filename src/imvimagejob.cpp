// SPDX-License-Identifier: LGPL-3.0-only
#include "imvimagejob.h"

#include <limits>

namespace Imv {

ImvImageJob::ImvImageJob(QObject *parent) : QObject(parent) {}

int ImvImageJob::sample(const QUrl &source, int maxWidth, int maxHeight) {
    if (source.isEmpty() || maxWidth <= 0 || maxHeight <= 0 ||
        m_lastId == std::numeric_limits<int>::max())
        return -1;
    const int id = ++m_lastId;
    QMetaObject::invokeMethod(
        this,
        [this, id] {
            emit finished(id, {{"stage", "decode"},
                               {"code", "not-implemented"},
                               {"detail", "Pixel decoding is not implemented yet"}});
        },
        Qt::QueuedConnection);
    return id;
}

} // namespace Imv
