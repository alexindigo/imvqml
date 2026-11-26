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
#include <algorithm>
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
    struct Invalidation {
        QString directory;
        QByteArray fingerprint;
        Evicted done;
        QDeadlineTimer deadline;

        bool matches(const QString &dir, QByteArrayView hash) const {
            return directory == dir && (fingerprint.isEmpty() || fingerprint == hash);
        }
    };
    QString directory;
    qint64 budget = 0;
    bool enabled = false;
    QByteArray version;
    quint64 epoch = 1;
    QHash<QString, std::weak_ptr<const QString>> leases;
    QList<Invalidation> invalidations;
    bool retryScheduled = false;

    bool invalidated(const QByteArray &fingerprint) const {
        return std::any_of(invalidations.cbegin(), invalidations.cend(), [&](const auto &request) {
            return request.matches(directory, fingerprint);
        });
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
                !invalidated(file.fileName().first(64).toLatin1()) &&
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
    m_thumbnailDeadline.setSingleShot(true);
    m_thumbnailDeadline.setTimerType(Qt::PreciseTimer);
    connect(&m_thumbnailDeadline, &QTimer::timeout, this, &ImageStore::pumpThumbnails);
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
                if (lock.tryLock(0))
                    d->makeSpace(0);
            }
        });
}

void ImageStore::lookup(Fd input, DecodeSpec spec, std::function<void(Lookup)> done) {
    queueLookup([fd = std::make_shared<const Fd>(std::move(input))] { return fd; }, spec,
                std::move(done));
}

void ImageStore::lookup(std::weak_ptr<const Fd> input, DecodeSpec spec,
                        std::function<void(Lookup)> done) {
    queueLookup([input] { return input.lock(); }, spec, std::move(done));
}

void ImageStore::queueLookup(std::function<std::shared_ptr<const Fd>()> source, DecodeSpec spec,
                             std::function<void(Lookup)> done) {
    m_worker.start([this, source = std::move(source), spec, done = std::move(done),
                    epoch = m_epoch]() mutable {
        const auto input = source();
        if (!input)
            return;
        Lookup result;
        result.epoch = epoch;
        if (d->enabled && d->epoch == epoch && privateDirectory(d->directory)) {
            // Only immutable decode inputs may create a publishable lookup.
            const int seals = ::fcntl(input->get(), F_GET_SEALS);
            if (seals >= 0 && (seals & MemfdSeals) == MemfdSeals)
                result.fingerprint = digest(*input);
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
                if (d->invalidated(result.fingerprint)) {
                    result.path.clear();
                } else {
                    result.lease = d->acquire(result.path);
                    QLockFile lock(QDir(d->directory).filePath(".imvqml.lock"));
                    const Fd fd(lock.tryLock(0)
                                    ? ::open(QFile::encodeName(result.path).constData(),
                                             O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW)
                                    : -1);
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

void ImageStore::publish(Lookup lookup, Decoded decoded) {
    if (!m_enabled || lookup.path.isEmpty() || lookup.epoch != m_epoch ||
        !m_pixelWrites.tryAcquire())
        return;
    m_worker.start([this, permit = QSemaphoreReleaser(m_pixelWrites), lookup = std::move(lookup),
                    decoded = std::move(decoded)] {
        if (lookup.epoch != d->epoch)
            return;
        const auto data = encodeEntry(decoded, d->budget);
        if (data.isEmpty())
            return;
        QLockFile lock(QDir(d->directory).filePath(".imvqml.lock"));
        if (!lock.tryLock(0))
            return;
        if (d->invalidated(lookup.fingerprint) || !d->makeSpace(data.size(), lookup.path))
            return;
        QSaveFile file(lookup.path);
        if (file.open(QIODevice::WriteOnly) &&
            file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) &&
            file.write(data) == data.size())
            file.commit();
    });
}

void ImageStore::clear(Evicted done) { queueInvalidation({}, std::move(done)); }

void ImageStore::invalidate(Fd input, Evicted done) {
    // The shared wrapper keeps the callable copyable for the worker API.
    queueInvalidation(
        [fd = std::make_shared<Fd>(std::move(input))] { return OpenedSource{std::move(*fd)}; },
        std::move(done));
}

void ImageStore::invalidate(QUrl url, Evicted done) {
    queueInvalidation([url = std::move(url)] { return openSource(url); }, std::move(done));
}

void ImageStore::queueInvalidation(std::function<OpenedSource()> source, Evicted done) {
    m_worker.start(
        [this, source = std::move(source), done = std::move(done), epoch = ++m_epoch]() mutable {
            d->epoch = epoch;
            QByteArray fingerprint;
            if (source) {
                const auto input = source();
                fingerprint = input.fd ? digest(input.fd) : QByteArray{};
                if (input.error || fingerprint.isEmpty()) {
                    finishInvalidation(std::move(done),
                                       {0, 0, 0,
                                        input.error ? input.error
                                                    : Failure{"open", "unreadable",
                                                              "Could not identify cache source"}});
                    return;
                }
            }
            d->invalidations.append({d->directory, std::move(fingerprint), std::move(done),
                                     QDeadlineTimer(8000, Qt::PreciseTimer)});
            flushInvalidations();
        });
}

void ImageStore::flushInvalidations() {
    while (!d->invalidations.isEmpty()) {
        auto &request = d->invalidations.first();
        Eviction result;
        const QByteArray path = QFile::encodeName(request.directory);
        struct stat info{};
        if (!path.isEmpty()) {
            if (::stat(path.constData(), &info) != 0) {
                if (errno != ENOENT)
                    result.error = {"store", "write-failed", "Could not access cache directory"};
            } else if (!S_ISDIR(info.st_mode) || !QDir(request.directory).isReadable()) {
                result.error = {"store", "write-failed", "Could not read cache directory"};
            } else {
                QLockFile lock(QDir(request.directory).filePath(".imvqml.lock"));
                if (lock.tryLock(0)) {
                    for (const auto &file : entries(request.directory)) {
                        if (!request.matches(request.directory,
                                             file.fileName().first(64).toLatin1()))
                            continue;
                        if (!d->leases.value(file.filePath()).expired())
                            ++result.kept;
                        else if (QFile::remove(file.filePath()))
                            ++result.removed;
                        else
                            ++result.failed;
                    }
                    if (result.failed)
                        result.error = {"store", "write-failed",
                                        "Could not remove some cache entries"};
                } else if (lock.error() != QLockFile::LockFailedError) {
                    result.error = {"store", "write-failed", "Could not lock cache directory"};
                } else if (request.deadline.hasExpired()) {
                    result.error = {"store", "deadline", "Cache directory remained locked"};
                } else {
                    if (!std::exchange(d->retryScheduled, true))
                        QMetaObject::invokeMethod(
                            this,
                            [this] {
                                QTimer::singleShot(50, this, [this] {
                                    m_worker.start([this] {
                                        d->retryScheduled = false;
                                        flushInvalidations();
                                    });
                                });
                            },
                            Qt::QueuedConnection);
                    return;
                }
            }
        }
        auto completed = d->invalidations.takeFirst();
        finishInvalidation(std::move(completed.done), std::move(result));
    }
}

void ImageStore::finishInvalidation(Evicted done, Eviction result) {
    if (done)
        QMetaObject::invokeMethod(
            this, [done = std::move(done), result = std::move(result)] { done(result); },
            Qt::QueuedConnection);
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

void ImageStore::thumbnail(QString path, struct stat before, QImage pixels, SourceInfo info,
                           int size, std::weak_ptr<void> interest,
                           std::function<void(Failure, QUrl)> done) {
    constexpr int WaitMs = 8000;
    constexpr int QueueLimit = 64;
    pumpThumbnails();
    Thumbnail request{std::move(path),
                      before,
                      std::move(pixels),
                      std::move(info),
                      size,
                      std::move(interest),
                      std::move(done),
                      QDeadlineTimer(WaitMs, Qt::PreciseTimer)};
    if (m_thumbnails.size() >= QueueLimit) {
        finishThumbnail(std::move(request),
                        {"store", "deadline", "Thumbnail writer queue is full"});
        return;
    }
    m_thumbnails.append(std::move(request));
    pumpThumbnails();
}

void ImageStore::finishThumbnail(Thumbnail request, Failure failure, QUrl output) {
    QMetaObject::invokeMethod(
        this,
        [interest = std::move(request.interest), done = std::move(request.done),
         failure = std::move(failure), output = std::move(output)] {
            if (!interest.expired())
                done(failure, output);
        },
        Qt::QueuedConnection);
}

void ImageStore::pumpThumbnails() {
    m_thumbnails.removeIf([](const Thumbnail &request) { return request.interest.expired(); });
    const Failure deadline{"store", "deadline", "Thumbnail writer admission deadline expired"};
    while (!m_thumbnails.isEmpty() && m_thumbnails.first().deadline.hasExpired())
        finishThumbnail(m_thumbnails.takeFirst(), deadline);
    if (!m_writingThumbnail && !m_thumbnails.isEmpty()) {
        m_writingThumbnail = true;
        m_worker.start(
            [this, request = m_thumbnails.takeFirst(), deadline]() mutable {
                auto &[path, before, pixels, info, size, interest, done, timer] = request;
                const auto failure = timer.hasExpired() ? deadline : Failure{};
                const auto produce = [&]() -> QUrl {
                    struct stat current{};
                    const auto directoryKey = thumbnailBucket(size);
                    if (directoryKey.isEmpty() || pixels.isNull())
                        return {};
                    auto base = qEnvironmentVariable("XDG_CACHE_HOME");
                    if (!QDir::isAbsolutePath(base))
                        base = QDir::home().filePath(".cache");
                    const auto thumbnails = QDir(base).filePath("thumbnails");
                    const auto directory = QDir(thumbnails).filePath(directoryKey);
                    if (!privateDirectory(thumbnails) || !privateDirectory(directory))
                        return {};
                    const auto uri = QUrl::fromLocalFile(path).toEncoded();
                    const auto name =
                        QString::fromLatin1(
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
                    else if (info.format == "heif" || info.format == "avif" ||
                             info.format == "jxl" || info.format == "webp" ||
                             info.format == "jpeg" || info.format == "gif" ||
                             info.format == "tiff" || info.format == "png")
                        pixels.setText("Thumb::Mimetype", "image/" + info.format);
                    pixels.setText("Software", "imvqml");
                    QSaveFile file(output);
                    if (!file.open(QIODevice::WriteOnly) ||
                        !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
                        !pixels.save(&file, "PNG") ||
                        ::stat(QFile::encodeName(path).constData(), &current) ||
                        !same(before, current) || interest.expired() || !file.commit())
                        return {};
                    return QUrl::fromLocalFile(output);
                };
                const auto result = !failure && !interest.expired() ? produce() : QUrl{};
                QMetaObject::invokeMethod(
                    this,
                    [this, request = std::move(request), failure, result]() mutable {
                        m_writingThumbnail = false;
                        finishThumbnail(std::move(request),
                                        failure ? failure
                                        : result.isEmpty()
                                            ? Failure{"store", "write-failed",
                                                      "Could not publish freedesktop thumbnail"}
                                            : Failure{},
                                        result);
                        pumpThumbnails();
                    },
                    Qt::QueuedConnection);
            },
            1);
    }
    if (m_thumbnails.isEmpty())
        m_thumbnailDeadline.stop();
    else
        m_thumbnailDeadline.start(
            int(qMax<qint64>(1, m_thumbnails.first().deadline.remainingTime())));
}

ImageStore &imageStore() {
    static QPointer<ImageStore> instance;
    if (!instance)
        instance = new ImageStore(IMV_STORE_VERSION, QCoreApplication::instance());
    return *instance;
}
} // namespace Imv
