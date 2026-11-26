// SPDX-License-Identifier: LGPL-3.0-only
#include "image-store.h"
#include "store-codec.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Imv {
namespace {
bool same(const struct stat &a, const struct stat &b) {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
           a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

QByteArray libraryStamp(const char *path) {
    struct stat info{};
    if (::stat(path, &info))
        return "/absent";
    return '/' + QByteArray::number(info.st_ino) + '/' + QByteArray::number(info.st_size) + '/' +
           QByteArray::number(info.st_mtim.tv_sec) + '/' + QByteArray::number(info.st_mtim.tv_nsec);
}

QByteArray digest(const Fd &input) {
    struct stat before{}, after{};
    if (::fstat(input.get(), &before) || !S_ISREG(before.st_mode) ||
        before.st_size > MaxSourceBytes)
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    char block[64 * 1024];
    for (qint64 pos = 0; pos < before.st_size;) {
        const auto size =
            ::pread(input.get(), block, qMin<qint64>(sizeof block, before.st_size - pos), pos);
        if (size < 0 && errno == EINTR)
            continue;
        if (size <= 0)
            return {};
        pos += size;
        hash.addData(QByteArrayView(block, size));
    }
    return !::fstat(input.get(), &after) && same(before, after) ? hash.result().toHex()
                                                                : QByteArray{};
}

bool privateDirectory(const QString &path) {
    if (QFileInfo::exists(path))
        return QFileInfo(path).isDir();
    const QFileInfo info(path);
    return QDir().mkpath(info.absolutePath()) &&
           (::mkdir(QFile::encodeName(path).constData(), 0700) == 0 ||
            (errno == EEXIST && QFileInfo(path).isDir()));
}

QFileInfoList entries(const QString &directory) {
    static const QRegularExpression valid(R"(^[0-9a-f]{64}-[0-9a-f]{64}\.imvqoi$)");
    auto list = QDir(directory).entryInfoList({"*.imvqoi"}, QDir::Files | QDir::NoSymLinks,
                                              QDir::Time | QDir::Reversed);
    list.removeIf([](const QFileInfo &info) { return !valid.match(info.fileName()).hasMatch(); });
    return list;
}
} // namespace

struct ImageStore::Worker {
    QString directory;
    qint64 budget = 0;
    bool enabled = false;
    QByteArray version;
    quint64 epoch = 1;
    QHash<QString, std::weak_ptr<const QString>> leases;
    QHash<QString, QSet<QByteArray>> invalidations;

    // Called under the cross-process directory lock, before reading or writing
    // any cache entries. An empty fingerprint marks the whole directory.
    void flushInvalidations() {
        auto pending = invalidations.find(directory);
        if (pending == invalidations.end() || !QDir(directory).isReadable())
            return;
        QSet<QByteArray> failed;
        for (const auto &file : entries(directory)) {
            const auto fingerprint = file.fileName().first(64).toLatin1();
            if ((pending->contains(QByteArray{}) || pending->contains(fingerprint)) &&
                !QFile::remove(file.filePath()) && QFileInfo::exists(file.filePath()))
                failed.insert(fingerprint);
        }
        if (failed.isEmpty())
            invalidations.erase(pending);
        else
            *pending = std::move(failed);
    }

    bool invalidated(const QByteArray &fingerprint) const {
        const auto pending = invalidations.constFind(directory);
        return pending != invalidations.cend() &&
               (pending->contains(QByteArray{}) || pending->contains(fingerprint));
    }

    Lease acquire(const QString &path) {
        leases.removeIf([](const auto &item) { return item.value().expired(); });
        auto &entry = leases[path];
        auto lease = entry.lock();
        if (!lease) {
            lease = std::make_shared<const QString>(path);
            entry = lease;
        }
        return lease;
    }

    bool makeSpace(qint64 needed, const QString &replacing = {}) {
        const auto files = entries(directory);
        qint64 used = 0;
        for (const auto &file : files)
            if (file.filePath() != replacing)
                used += file.size();
        for (const auto &file : files) {
            if (used <= budget - needed)
                break;
            if (file.filePath() != replacing && leases.value(file.filePath()).expired() &&
                QFile::remove(file.filePath()))
                used -= file.size();
        }
        return used <= budget - needed;
    }
};

ImageStore::ImageStore(QByteArray version, QObject *parent)
    : QObject(parent), d(std::make_unique<Worker>()) {
    d->version = std::move(version);
    m_worker.setMaxThreadCount(1);
    m_worker.setExpiryTimeout(-1);
}

ImageStore::~ImageStore() {
    m_worker.clear();
    m_worker.waitForDone();
}

void ImageStore::configure(QString directory, qint64 budget, bool enabled) {
    m_enabled = enabled && !directory.isEmpty() && budget > 0;
    m_worker.start(
        [this, directory = std::move(directory), budget, enabled = m_enabled, epoch = ++m_epoch] {
            d->epoch = epoch;
            d->directory = directory;
            d->budget = budget;
            d->enabled = enabled;
            if (enabled && privateDirectory(directory)) {
                QLockFile lock(QDir(directory).filePath(".imvqml.lock"));
                if (lock.tryLock(0)) {
                    d->flushInvalidations();
                    d->makeSpace(0);
                }
            }
        });
}

void ImageStore::lookup(Fd input, DecodeSpec spec, std::function<void(Lookup)> done) {
    m_worker.start(
        [this, input = std::move(input), spec, done = std::move(done), epoch = m_epoch]() mutable {
            Lookup result;
            result.epoch = epoch;
            if (d->enabled && d->epoch == epoch && privateDirectory(d->directory)) {
                result.fingerprint = digest(input);
                if (!result.fingerprint.isEmpty()) {
                    auto version =
                        d->version + libraryStamp(IMV_VIPS_PATH) + libraryStamp(IMV_MPV_PATH);
                    if (spec.mode == DecodeSpec::Animation || spec.mode == DecodeSpec::Pixels)
                        version += encode(spec);
                    const auto parameter =
                        QCryptographicHash::hash(version, QCryptographicHash::Sha256).toHex();
                    result.path = QDir(d->directory)
                                      .filePath(QString::fromLatin1(result.fingerprint + '-' +
                                                                    parameter + ".imvqoi"));
                    result.lease = d->acquire(result.path);
                    QLockFile lock(QDir(d->directory).filePath(".imvqml.lock"));
                    if (lock.tryLock(0)) {
                        d->flushInvalidations();
                        const Fd fd(d->invalidated(result.fingerprint)
                                        ? -1
                                        : ::open(QFile::encodeName(result.path).constData(),
                                                 O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW));
                        QFile file;
                        struct stat stat{};
                        if (fd && !::fstat(fd.get(), &stat) && S_ISREG(stat.st_mode) &&
                            stat.st_size <= qMin(d->budget, MaxEntryBytes) &&
                            file.open(fd.get(), QIODevice::ReadOnly)) {
                            result.value = decodeEntry(file.read(stat.st_size), spec);
                            if (result.value) {
                                const timespec time[]{{0, UTIME_OMIT}, {0, UTIME_NOW}};
                                ::futimens(fd.get(), time);
                            } else {
                                QFile::remove(result.path);
                            }
                        }
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, done = std::move(done), result = std::move(result)]() mutable {
                    if (result.epoch != m_epoch)
                        result = {};
                    done(std::move(result));
                },
                Qt::QueuedConnection);
        });
}

void ImageStore::publish(Lookup lookup, Fd input, Decoded decoded) {
    if (!m_enabled || lookup.path.isEmpty() || lookup.epoch != m_epoch)
        return;
    m_worker.start(
        [this, lookup = std::move(lookup), input = std::move(input), decoded = std::move(decoded)] {
            if (lookup.epoch != d->epoch || digest(input) != lookup.fingerprint)
                return;
            const auto data = encodeEntry(decoded, d->budget);
            if (data.isEmpty())
                return;
            QLockFile lock(QDir(d->directory).filePath(".imvqml.lock"));
            if (!lock.tryLock(0))
                return;
            d->flushInvalidations();
            if (d->invalidated(lookup.fingerprint) || !d->makeSpace(data.size(), lookup.path))
                return;
            QSaveFile file(lookup.path);
            if (file.open(QIODevice::WriteOnly) &&
                file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
                file.write(data) == data.size())
                file.commit();
        });
}

void ImageStore::clear() { invalidate({}); }

void ImageStore::invalidate(Fd input) {
    m_worker.start([this, input = std::move(input), epoch = ++m_epoch] {
        d->epoch = epoch;
        if (d->directory.isEmpty() || !QFileInfo::exists(d->directory))
            return;
        const auto fingerprint = input ? digest(input) : QByteArray{};
        if (input && fingerprint.isEmpty()) {
            qWarning("imvqml: Could not identify source for cache invalidation");
            return;
        }
        auto &pending = d->invalidations[d->directory];
        if (fingerprint.isEmpty())
            pending.clear();
        if (!pending.contains(QByteArray{}))
            pending.insert(fingerprint);
        QLockFile lock(QDir(d->directory).filePath(".imvqml.lock"));
        if (lock.tryLock(200))
            d->flushInvalidations();
        if (d->invalidations.contains(d->directory))
            qWarning("imvqml: Cache invalidation pending; affected entries will not be reused");
    });
}

QString thumbnailBucket(int size) {
    switch (size) {
    case 128:
        return "normal";
    case 256:
        return "large";
    case 512:
        return "x-large";
    case 1024:
        return "xx-large";
    default:
        return {};
    }
}

void ImageStore::thumbnail(QString path, Fd input, QImage pixels, SourceInfo info, int size,
                           std::function<void(Failure, QUrl)> done) {
    m_worker.start([this, path = std::move(path), input = std::move(input),
                    pixels = std::move(pixels), info = std::move(info), size,
                    done = std::move(done)]() mutable {
        const auto produce = [&]() -> QUrl {
            struct stat before{}, current{};
            const auto directoryKey = thumbnailBucket(size);
            if (directoryKey.isEmpty() || ::fstat(input.get(), &before) || pixels.isNull())
                return {};
            auto base = qEnvironmentVariable("XDG_CACHE_HOME");
            if (!QDir::isAbsolutePath(base))
                base = QDir::home().filePath(".cache");
            const auto thumbnails = QDir(base).filePath("thumbnails");
            const auto directory = QDir(thumbnails).filePath(directoryKey);
            if (!privateDirectory(thumbnails) || !privateDirectory(directory))
                return {};
            const auto uri = QUrl::fromLocalFile(path).toEncoded();
            const auto name = QString::fromLatin1(
                                  QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex()) +
                              ".png";
            if (QDir(thumbnails).relativeFilePath(path).startsWith("../") == false)
                return {};
            const auto output = QDir(directory).filePath(name);
            pixels.setText("Thumb::URI", QString::fromUtf8(uri));
            pixels.setText("Thumb::MTime", QString::number(before.st_mtim.tv_sec));
            pixels.setText("Thumb::Size", QString::number(before.st_size));
            pixels.setText("Thumb::Image::Width", QString::number(info.size.width()));
            pixels.setText("Thumb::Image::Height", QString::number(info.size.height()));
            if (info.format == "svg")
                pixels.setText("Thumb::Mimetype", "image/svg+xml");
            else if (info.format == "heif" || info.format == "avif" || info.format == "jxl" ||
                     info.format == "webp" || info.format == "jpeg" || info.format == "gif" ||
                     info.format == "tiff" || info.format == "png")
                pixels.setText("Thumb::Mimetype", "image/" + info.format);
            pixels.setText("Software", "imvqml");
            QSaveFile file(output);
            if (!file.open(QIODevice::WriteOnly) ||
                !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
                !pixels.save(&file, "PNG") ||
                ::stat(QFile::encodeName(path).constData(), &current) || !same(before, current) ||
                !file.commit())
                return {};
            return QUrl::fromLocalFile(output);
        };
        const auto result = produce();
        QMetaObject::invokeMethod(
            this,
            [done = std::move(done), result] {
                done(result.isEmpty() ? Failure{"store", "write-failed",
                                                "Could not publish freedesktop thumbnail"}
                                      : Failure{},
                     result);
            },
            Qt::QueuedConnection);
    });
}

ImageStore &imageStore() {
    static QPointer<ImageStore> instance;
    if (!instance)
        instance = new ImageStore(IMV_STORE_VERSION, QCoreApplication::instance());
    return *instance;
}
} // namespace Imv
