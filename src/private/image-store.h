// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "decode-client.h"
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
    void publish(Lookup lookup, Fd input, Decoded decoded);
    void clear();
    void invalidate(Fd input);
    bool enabled() const { return m_enabled; }
    quint64 epoch() const { return m_epoch; }
    void thumbnail(QString path, Fd input, QImage pixels, SourceInfo info, int size,
                   std::function<void(Failure, QUrl)> done);

private:
    struct Worker;
    std::unique_ptr<Worker> d;
    QThreadPool m_worker;
    quint64 m_epoch = 1;
    bool m_enabled = false;
};

ImageStore &imageStore();
QString thumbnailBucket(int size);

} // namespace Imv
