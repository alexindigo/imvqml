// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "decode-client.h"
#include <QSemaphore>
#include <QThreadPool>

namespace Imv {

class ImageStore : public QObject {
public:
    using Lease = std::shared_ptr<const QString>;
    struct Lookup {
        QString path;
        QByteArray fingerprint;
        Lease lease;
        std::optional<Decoded> value;
        quint64 epoch = 0;
    };
    static constexpr qint64 DefaultBudget = 512ll * 1024 * 1024;
    explicit ImageStore(QByteArray version, QObject *parent = nullptr);
    ~ImageStore() override;
    void configure(QString directory, qint64 budget, bool enabled);
    void lookup(Fd input, DecodeSpec spec, std::function<void(Lookup)> done);
    void lookup(std::weak_ptr<const Fd> input, DecodeSpec spec, std::function<void(Lookup)> done);
    void publish(Lookup lookup, Decoded decoded);
    void clear();
    void invalidate(Fd input);
    void invalidate(QUrl url);
    bool enabled() const { return m_enabled; }
    quint64 epoch() const { return m_epoch; }
    void thumbnail(QString path, struct stat revision, QImage pixels, SourceInfo info, int size,
                   std::function<void(Failure, QUrl)> done);

private:
    struct Worker;
    std::unique_ptr<Worker> d;
    QSemaphore m_pixelWrites{2};
    QThreadPool m_worker;
    void queueLookup(std::function<std::shared_ptr<const Fd>()> source, DecodeSpec spec,
                     std::function<void(Lookup)> done);
    void queueInvalidation(std::function<OpenedSource()> source);
    quint64 m_epoch = 1;
    bool m_enabled = false;
};

ImageStore &imageStore();
QString thumbnailBucket(int size);

} // namespace Imv
