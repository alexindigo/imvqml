// SPDX-License-Identifier: LGPL-3.0-only
#include "decode-client.h"
#include "video-signature.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QPointer>
#include <QThreadPool>
#include <QtEndian>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Imv {
class SourceWorker : public QObject {
    Q_OBJECT
public:
    explicit SourceWorker(QObject *parent) : QObject(parent) { pool.setMaxThreadCount(2); }
    int outstanding = 0;
    QThreadPool pool;

signals:
    void capacityReleased();
};

namespace {
OpenedSource fail(QString stage, QString code, QString detail) {
    return {{}, {}, {std::move(stage), std::move(code), std::move(detail)}};
}

using Mapping = std::shared_ptr<const uchar>;
struct SourceTask {
    PreparationInput input;
    SourceReady ready;
    std::atomic_bool live{true};
};

SourceWorker &sourceWorker() {
    static QPointer<SourceWorker> service;
    if (!service)
        service = new SourceWorker(QCoreApplication::instance());
    return *service;
}
QImage imageView(const Mapping &mapping, QSize size, qint64 offset) {
    return QImage(
        mapping.get() + offset, size.width(), size.height(), size.width() * 4,
        QImage::Format_RGBA8888, [](void *ptr) { delete static_cast<Mapping *>(ptr); },
        new Mapping(mapping));
}

bool sealedOutput(int fd, qint64 bytes) {
    struct stat info{};
    const int seals = ::fcntl(fd, F_GET_SEALS);
    return bytes > 0 && seals >= 0 && (seals & MemfdSeals) == MemfdSeals && !::fstat(fd, &info) &&
           S_ISREG(info.st_mode) && info.st_size == bytes;
}

OpenedSource openData(const QUrl &url, SourceLimit limit) {
    if (!url.isValid() || url.hasFragment() || !url.authority().isEmpty())
        return fail("admission", "invalid-source", "Malformed data URL");
    // QUrl already owns its encoded text. Materialize only one normalized
    // representation; decode it into the sealed file in bounded blocks.
    const auto text = url.toString(QUrl::RemoveScheme | QUrl::FullyEncoded);
    const auto comma = text.indexOf(',');
    if (comma < 0 || comma > 1024)
        return fail("admission", "invalid-source", "Data URL lacks a bounded metadata field");
    const bool base64 = QStringView(text).first(comma).endsWith(u";base64");
    const auto hex = [](ushort c) {
        return c >= '0' && c <= '9'   ? int(c - '0')
               : c >= 'A' && c <= 'F' ? int(c - 'A' + 10)
               : c >= 'a' && c <= 'f' ? int(c - 'a' + 10)
                                      : -1;
    };
    auto fd = newMemfd("imvqml-data-source");
    if (!fd)
        return fail("open", "unreadable", "Could not allocate the byte descriptor");
    QByteArray block;
    constexpr qsizetype BlockSize = 64 * 1024;
    block.reserve(BlockSize);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 size = 0;
    bool padded = false;
    bool video = false;
    for (qsizetype i = comma + 1; i <= text.size();) {
        if (i < text.size()) {
            auto c = text[i++].unicode();
            if (c == '%' && i + 1 < text.size()) {
                const int high = hex(text[i].unicode()), low = hex(text[i + 1].unicode());
                if (high >= 0 && low >= 0) {
                    c = ushort((high << 4) | low);
                    i += 2;
                }
            }
            if (padded || c > 255)
                return fail("admission", "invalid-source", "Malformed data URL payload");
            block.append(char(c));
            if (block.size() != BlockSize)
                continue;
        } else {
            ++i;
            if (block.isEmpty())
                break;
        }
        if (base64) {
            padded = block.contains('=');
            auto decoded = QByteArray::fromBase64Encoding(std::move(block),
                                                          QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded)
                return fail("admission", "invalid-source", "Malformed base64 data URL");
            block = std::move(decoded.decoded);
        }
        if (!size && limit == SourceLimit::Decode)
            video = videoDemuxer(QByteArrayView(block).left(VideoSignatureBytes));
        if (!video && block.size() > MaxSourceBytes - size)
            return fail("admission", "too-large", "Source exceeds 256 MiB");
        if (!writeAll(fd.get(), block.constData(), block.size(), size))
            return fail("open", "unreadable", "Could not write the byte descriptor");
        size += block.size();
        hash.addData(block);
        block.clear();
    }
    if (::fcntl(fd.get(), F_ADD_SEALS, MemfdSeals) < 0)
        return fail("open", "unreadable", "Could not seal the byte descriptor");
    return {std::move(fd), "bytes/" + hash.result().toHex(), {}};
}

Failure translate(DecodeCode code, QString detail) {
    switch (code) {
    case DecodeCode::Unreadable:
        return {"open", "unreadable", detail};
    case DecodeCode::Unsupported:
        return {"inspect", "unsupported-format", detail};
    case DecodeCode::InvalidHeader:
        return {"inspect", "invalid-image", detail};
    case DecodeCode::SourceLimit:
        return {"admission", "too-large", detail};
    case DecodeCode::PixelLimit:
        return {"inspect", "too-large", detail};
    case DecodeCode::Decode:
        return {"decode", "invalid-image", detail};
    case DecodeCode::Output:
        return {"decode", "output-failed", detail};
    case DecodeCode::Ok:
        return {};
    }
    Q_UNREACHABLE();
}
} // namespace

QImage AnimationFrames::frame(int index) const {
    if (!mapping || index < 0 || index >= count)
        return {};
    return imageView(mapping, size, index * (rgbaBytes(size) + 4) + 4);
}

int AnimationFrames::delay(int index) const {
    if (!mapping || index < 0 || index >= count)
        return 0;
    return qMax(1, qFromLittleEndian<qint32>(mapping.get() + index * (rgbaBytes(size) + 4)));
}

Fd duplicateSource(const Fd &fd) { return Fd(::fcntl(fd.get(), F_DUPFD_CLOEXEC, 0)); }

QString localPath(const QUrl &url) {
    if (!url.isValid() || !url.host().isEmpty() || url.hasQuery() || url.hasFragment())
        return {};
    const auto path = url.isLocalFile()        ? url.toLocalFile()
                      : url.scheme().isEmpty() ? url.path()
                                               : QString{};
    return path.contains(QChar::Null) ? QString{} : path;
}

CapturedSource captureSource(const QUrl &url, SourceLimit limit) {
    if (url.scheme() == "data")
        return {url, {}, {}, {}, limit};
    const auto path = localPath(url);
    if (path.isEmpty())
        return {{},
                {},
                {},
                {"admission", "invalid-source",
                 "Only local paths, file URLs and data URLs are supported here"}};
    Fd fd(::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    struct stat info{};
    if (!fd || ::fstat(fd.get(), &info) != 0 || !S_ISREG(info.st_mode))
        return {{}, {}, {}, {"open", "unreadable", "Cannot open a readable regular file"}};
    if (limit == SourceLimit::Strict && info.st_size > MaxSourceBytes)
        return {{}, {}, {}, {"admission", "too-large", "Source exceeds 256 MiB"}};
    return {{}, std::move(fd), info, {}, limit};
}

namespace {
bool sameContentStat(const struct stat &a, const struct stat &b) {
    return a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
           a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
}

OpenedSource materialize(CapturedSource source) {
    if (source.error)
        return {{}, {}, std::move(source.error)};
    if (!source.fd)
        return openData(source.data, source.limit);
    const auto &fd = source.fd;
    const auto &info = source.file;
    // Rename/unlink can change ctime without changing the owned file's pixels.
    struct stat before{};
    if (::fstat(fd.get(), &before) || !sameContentStat(info, before))
        return fail("open", "unreadable", "Source was modified before snapshot");
    const auto number = [](auto value) { return QByteArray::number(value); };
    auto identity = "file/" + number(info.st_dev) + '/' + number(info.st_ino) + '/' +
                    number(info.st_size) + '/' + number(info.st_mtim.tv_sec) + '/' +
                    number(info.st_mtim.tv_nsec) + '/' + number(info.st_ctim.tv_sec) + '/' +
                    number(info.st_ctim.tv_nsec);
    // A disk video can be arbitrarily large. Only the helper identifies and
    // streams over-limit files; it never sends an unsealed input to an image
    // reader. The persistent store already skips hashing over-limit inputs.
    if (info.st_size > MaxSourceBytes)
        return {std::move(source.fd), std::move(identity), {}, info};
    auto snapshot = newMemfd("imvqml-file-source");
    if (!snapshot)
        return fail("open", "unreadable", "Could not create a source snapshot");
    char block[64 * 1024];
    for (qint64 position = 0; position < info.st_size;) {
        const auto count =
            ::pread(fd.get(), block, qMin<qint64>(sizeof block, info.st_size - position), position);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || !writeAll(snapshot.get(), block, count, position))
            return fail("open", "unreadable", "Could not read the opened source revision");
        position += count;
    }
    struct stat after{};
    // Retention permits the producer to unlink throughout the copy. That can
    // change ctime alone. In-place edits are outside the caller's contract.
    if (::fstat(fd.get(), &after) || !sameContentStat(before, after) ||
        (source.limit == SourceLimit::Decode &&
         (before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
          before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)) ||
        ::fcntl(snapshot.get(), F_ADD_SEALS, MemfdSeals) < 0)
        return fail("open", "unreadable", "Could not seal an unchanged source revision");
    return {std::move(snapshot), std::move(identity), {}, info};
}
} // namespace

OpenedSource openSource(const QUrl &url) { return materialize(captureSource(url)); }

OpenedSource openSource(const QByteArray &bytes) {
    if (bytes.size() > MaxSourceBytes &&
        !videoDemuxer(QByteArrayView(bytes).left(VideoSignatureBytes)))
        return fail("admission", "too-large", "Source exceeds 256 MiB");
    auto fd = newMemfd("imvqml-source");
    if (!fd || !writeAll(fd.get(), bytes.constData(), bytes.size()) ||
        ::fcntl(fd.get(), F_ADD_SEALS, MemfdSeals) < 0)
        return fail("open", "unreadable", "Could not materialize the immutable byte source");
    return {std::move(fd),
            "bytes/" + QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(),
            {}};
}

std::shared_ptr<void> prepareSource(PreparationInput &input, SourceReady ready) {
    auto &service = sourceWorker();
    constexpr int SourceCapacity = 4;
    if (service.outstanding >= SourceCapacity)
        return {};
    auto task = std::shared_ptr<SourceTask>(new SourceTask{std::move(input), std::move(ready)});
    ++service.outstanding;
    service.pool.start([owner = &service, weak = std::weak_ptr(task)] {
        const auto task = weak.lock();
        OpenedSource result;
        if (task && task->live) {
            if (auto *captured = std::get_if<CapturedSource>(&task->input))
                result = materialize(std::move(*captured));
            else
                result = std::visit([](const auto &input) { return openSource(input); },
                                    std::get<SourceInput>(task->input));
            task->input = SourceInput{};
        }
        QMetaObject::invokeMethod(
            owner,
            [owner, task, result = std::move(result)]() mutable {
                --owner->outstanding;
                if (task && task->live)
                    task->ready(std::move(result));
                emit owner->capacityReleased();
            },
            Qt::QueuedConnection);
    });
    return {task.get(), [task = std::move(task)](void *) { task->live = false; }};
}

void onSourceCapacity(QObject *receiver, std::function<void()> released) {
    QObject::connect(&sourceWorker(), &SourceWorker::capacityReleased, receiver,
                     std::move(released));
}

Pool &decoderPool() {
    static QPointer<Pool> pool;
    if (!pool)
        pool = new Pool(QCoreApplication::instance());
    return *pool;
}

Pool::Ticket submitDecode(Pool &pool, Fd input, DecodeSpec spec,
                          std::function<void(Decoded)> done) {
    if (!input || !spec.valid())
        return {Pool::Admission::Invalid};
    Frame frame{Kind::Request, 0, encode(spec), {}};
    frame.fds.emplace_back(std::move(input));
    auto output = std::make_shared<Fd>();
    if (spec.mode != DecodeSpec::Inspect) {
        *output = newPixelOutput(spec);
        if (!*output)
            return {Pool::Admission::Invalid};
        auto sendFd = duplicateSource(*output);
        if (!sendFd)
            return {Pool::Admission::Invalid};
        frame.fds.emplace_back(std::move(sendFd));
    }
    return pool.submit(
        std::move(frame),
        std::chrono::milliseconds(spec.mode >= DecodeSpec::Animation ? AnimationDeadlineMs
                                                                     : StillDeadlineMs),
        [done = std::move(done), output](JobReply reply) {
            if (reply.error) {
                done({{}, {}, std::move(reply.error)});
                return;
            }
            DecodeReport report;
            if (!decode(reply.frame.body, report)) {
                done({{}, {}, {"helper", "protocol-error", "Invalid decoder response"}});
                return;
            }
            if (report.code != DecodeCode::Ok) {
                done({{}, {}, translate(report.code, std::move(report.detail))});
                return;
            }
            Decoded result{std::move(report.source),
                           {},
                           {},
                           {},
                           report.layout == DecodeReport::Stream,
                           report.delay};
            if (*output) {
                const auto bytes = report.outputBytes();
                auto *pixels = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, output->get(), 0);
                if (pixels == MAP_FAILED) {
                    done({{}, {}, {"decode", "output-failed", "Could not map sealed pixels"}});
                    return;
                }
                Mapping mapping(static_cast<const uchar *>(pixels),
                                [pixels, bytes](const uchar *) { ::munmap(pixels, bytes); });
                if (report.layout == DecodeReport::Batch) {
                    result.animation = {mapping, report.output, result.source.frames};
                    result.pixels = result.animation.frame(0);
                } else {
                    result.pixels = imageView(mapping, report.output, 0);
                }
            }
            done(std::move(result));
        },
        [spec, output](const Frame &response) {
            DecodeReport report;
            if (!response.fds.empty() || !decode(response.body, report))
                return false;
            if (report.code != DecodeCode::Ok)
                return true;
            if (spec.mode == DecodeSpec::Inspect)
                return report.layout == DecodeReport::Single && report.output == QSize(0, 0);
            if (spec.mode != DecodeSpec::Animation && report.layout != DecodeReport::Single)
                return false;
            if (spec.mode == DecodeSpec::FrameAt &&
                (!report.delay || spec.page >= report.source.frames))
                return false;
            return report.output.width() <= spec.bounds.width() &&
                   report.output.height() <= spec.bounds.height() &&
                   sealedOutput(output->get(), report.outputBytes());
        });
}

QVariantMap asMap(const SourceInfo &source) {
    return {{"format", source.format},
            {"width", source.size.width()},
            {"height", source.size.height()},
            {"frames", source.frames},
            {"loop", source.loop}};
}

QVariantMap asMap(const Failure &failure) {
    return {{"stage", failure.stage}, {"code", failure.code}, {"detail", failure.detail}};
}
} // namespace Imv

#include "decode-client.moc"
