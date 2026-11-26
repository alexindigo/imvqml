// SPDX-License-Identifier: LGPL-3.0-only
#include "export.h"
#include <QFile>
#include <QTemporaryDir>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <webp/encode.h>

namespace Imv {
namespace {
std::atomic<ExportObserver> exportObserver;
}

void observeExports(ExportObserver observer) { exportObserver.store(observer); }

Failure stageImage(const QString &destination, const QString &format, const QImage &image,
                   std::atomic<Publication> &phase) {
    auto failure = [](QString code, QString detail) {
        return Failure{"store", std::move(code), std::move(detail)};
    };
    if (phase.load() != Publication::Staging)
        return {};
    const auto slash = destination.lastIndexOf('/');
    const auto filename = QFile::encodeName(destination.mid(slash + 1));
    const auto parent = QFile::encodeName(destination.left(slash + 1));
    Fd directory(::open(parent.constData(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!directory)
        return failure("write-failed", "Cannot open staging directory");
    QTemporaryDir workspace(QStringLiteral("/proc/self/fd/%1/.imvqml-XXXXXX").arg(directory.get()));
    if (!workspace.isValid())
        return failure("write-failed", workspace.errorString());
    QFile staged(workspace.filePath("pixels"));
    if (!staged.open(QIODevice::NewOnly | QIODevice::WriteOnly,
                     QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return failure("write-failed", staged.errorString());
    if (format == "png") {
        if (!image.save(&staged, "PNG"))
            return failure("write-failed", "Cannot encode PNG");
    } else {
        uint8_t *bytes = nullptr;
        const auto size = WebPEncodeLosslessRGBA(image.constBits(), image.width(), image.height(),
                                                 image.bytesPerLine(), &bytes);
        const std::unique_ptr<uint8_t, decltype(&WebPFree)> encoded(bytes, WebPFree);
        if (!size || staged.write(reinterpret_cast<const char *>(bytes), size) != qint64(size))
            return failure("write-failed", "Cannot encode lossless WebP");
    }
    if (!staged.flush())
        return failure("write-failed", staged.errorString());
    const auto descriptor = "/proc/self/fd/" + QByteArray::number(staged.handle());
    const auto observe = exportObserver.load();
    if (observe)
        observe(Publication::Staging, staged.fileName());
    auto expected = Publication::Staging;
    if (!phase.compare_exchange_strong(expected, Publication::Publishing))
        return {};
    if (observe)
        observe(Publication::Publishing, staged.fileName());
    if (::linkat(AT_FDCWD, descriptor.constData(), directory.get(), filename.constData(),
                 AT_SYMLINK_FOLLOW) < 0) {
        if (errno == EEXIST)
            return failure("already-exists", "Destination already exists");
        return failure("write-failed", "Cannot publish staged image");
    }
    return {};
}
} // namespace Imv
