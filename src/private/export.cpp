// SPDX-License-Identifier: LGPL-3.0-only
#include "export.h"
#include "decode-wire.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <webp/encode.h>

namespace Imv {
namespace {
std::atomic<ExportObserver> exportObserver;

Failure writeFailed(QString detail) { return {"store", "write-failed", std::move(detail)}; }

template <typename Write>
Failure stageFile(const QString &destination, std::atomic<Publication> &phase, Write write) {
    if (phase.load() != Publication::Staging)
        return {};
    const auto slash = destination.lastIndexOf('/');
    const auto filename = QFile::encodeName(destination.mid(slash + 1));
    const auto parent = QFile::encodeName(destination.left(slash + 1));
    Fd directory(::open(parent.constData(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!directory)
        return writeFailed("Cannot open staging directory");
    QTemporaryDir workspace(QStringLiteral("/proc/self/fd/%1/.imvqml-XXXXXX").arg(directory.get()));
    if (!workspace.isValid())
        return writeFailed(workspace.errorString());
    QFile staged(workspace.filePath("payload"));
    if (!staged.open(QIODevice::NewOnly | QIODevice::ReadWrite,
                     QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return writeFailed(staged.errorString());
    if (auto error = write(staged); error || phase.load() != Publication::Staging)
        return error;
    if (!staged.flush())
        return writeFailed(staged.errorString());
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
            return {"store", "already-exists", "Destination already exists"};
        return writeFailed("Cannot publish staged image");
    }
    return {};
}
} // namespace

void observeExports(ExportObserver observer) { exportObserver.store(observer); }

Failure stageImage(const QString &destination, const QString &format, const QImage &image,
                   std::atomic<Publication> &phase) {
    return stageFile(destination, phase, [&](QFile &staged) -> Failure {
        if (format == "png") {
            if (!image.save(&staged, "PNG"))
                return writeFailed("Cannot encode PNG");
        } else {
            uint8_t *bytes = nullptr;
            const auto size = WebPEncodeLosslessRGBA(image.constBits(), image.width(),
                                                     image.height(), image.bytesPerLine(), &bytes);
            const std::unique_ptr<uint8_t, decltype(&WebPFree)> encoded(bytes, WebPFree);
            if (!size || staged.write(reinterpret_cast<const char *>(bytes), size) != qint64(size))
                return writeFailed("Cannot encode lossless WebP");
        }
        return {};
    });
}

RetainedFile stageOriginal(const QString &directory, const Fd *original, const QImage &provider,
                           std::atomic<Publication> &phase) {
    RetainedFile result{
        QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces))};
    result.error = stageFile(result.path, phase, [&](QFile &staged) -> Failure {
        // The decoder inspected the same sealed descriptor. Providers have no
        // encoded original; hash the lossless PNG we write from their snapshot.
        if (!original && (!provider.save(&staged, "PNG") || !staged.flush()))
            return writeFailed("Cannot stage provider PNG");
        const int fd = original ? original->get() : staged.handle();
        struct stat info{};
        if (::fstat(fd, &info) != 0 || info.st_size < 0)
            return {"open", "unreadable", "Cannot read the snapshot size"};
        if (info.st_size > MaxSourceBytes)
            return {"admission", "too-large", "Retained bytes exceed 256 MiB"};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        char block[64 * 1024];
        while (result.size < info.st_size) {
            if (phase.load() != Publication::Staging)
                return {};
            const auto count = ::pread(
                fd, block, qMin<qint64>(sizeof block, info.st_size - result.size), result.size);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return {"open", "unreadable", "Cannot read the retained snapshot"};
            if (original && staged.write(block, count) != count)
                return writeFailed(staged.errorString());
            hash.addData(QByteArrayView(block, count));
            result.size += count;
        }
        result.sha256 = QString::fromLatin1(hash.result().toHex());
        return {};
    });
    return result;
}
} // namespace Imv
