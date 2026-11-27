// SPDX-License-Identifier: LGPL-3.0-only
#pragma once
#include "pool.h"
#include <QImage>
#include <atomic>

namespace Imv {
enum class Publication { Staging, Cancelled, Publishing };
using ExportObserver = void (*)(Publication, const QString &);
void observeExports(ExportObserver observer);
Failure stageImage(const QString &destination, const QString &format, const QImage &image,
                   std::atomic<Publication> &phase);
struct RetainedFile {
    QString path;
    qint64 size = 0;
    QString sha256;
    Failure error;
};
RetainedFile stageOriginal(const QString &directory, const Fd *original, const QImage &provider,
                           std::atomic<Publication> &phase);
} // namespace Imv
