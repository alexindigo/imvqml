// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "decode-client.h"
#include <QHash>
#include <QList>
#include <QPointer>

namespace Imv {

struct ImageAsset {
    QByteArray key;
    SourceInfo info;
    QImage pixels;
};
using Asset = std::shared_ptr<const ImageAsset>;

class ImagePipeline : public QObject {
public:
    using Done = std::function<void(Asset, Failure)>;
    // Destroying this handle synchronously revokes GUI delivery interest.
    using Interest = std::shared_ptr<const Done>;

    explicit ImagePipeline(Pool &pool, QObject *parent = nullptr);
    Interest request(OpenedSource source, DecodeSpec spec, Done done);
    Asset providerAsset(QImage pixels, QSize original, DecodeSpec spec);
    quint64 submissions() const { return m_submissions; }

private:
    struct Pending {
        QByteArray key;
        Fd input;
        DecodeSpec spec;
        int ticket = 0;
        QList<std::weak_ptr<const Done>> listeners;
    };
    Interest interest(Done done, const std::shared_ptr<Pending> &pending = {});
    void finish(const std::shared_ptr<Pending> &pending, Decoded decoded);
    void pumpLater();
    void pump();
    Asset exchangeAsset(ImageAsset asset);
    void prune();

    Pool &m_pool;
    QHash<QByteArray, std::shared_ptr<Pending>> m_pending;
    QList<std::weak_ptr<Pending>> m_waiting;
    QHash<QByteArray, std::weak_ptr<const ImageAsset>> m_assets;
    bool m_scheduled = false;
    quint64 m_submissions = 0;
};

ImagePipeline &imagePipeline();

} // namespace Imv
