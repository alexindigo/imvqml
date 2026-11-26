// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "decode-client.h"
#include "image-provider.h"
#include "image-store.h"
#include <QDeadlineTimer>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QTimer>

namespace Imv {

struct ImageAsset {
    QByteArray key;
    SourceInfo info;
    QImage pixels;
    AnimationFrames animation;
    std::shared_ptr<const Fd> stream;
    DecodeSpec spec;
    int delay = 0;
    bool transient = false;
    ImageStore::Lease storeLease;
    std::optional<struct stat> file;
};
using Asset = std::shared_ptr<const ImageAsset>;

class ImagePipeline : public QObject {
    struct Pending;

public:
    using Done = std::function<void(Asset, Failure)>;
    class Listener {
        friend class ImagePipeline;
        Done done;
        std::weak_ptr<Pending> pending;
        bool visible = false;
    };
    // Destroying this handle synchronously revokes GUI delivery interest.
    using Interest = std::shared_ptr<Listener>;
    static constexpr int QueueLimit = 128;
    static constexpr int QueueWaitMs = 8000;

    explicit ImagePipeline(Pool &pool, QObject *parent = nullptr, int waitMs = QueueWaitMs);
    Interest request(std::variant<SourceInput, OpenedSource> source, DecodeSpec spec, Done done,
                     bool visible = true);
    // Only hard queue overflow rejects synchronously. The caller starts capture
    // before returning its ticket; a provider response may resolve later.
    enum class Explicit { OneShot, Thumbnail };
    Interest requestExplicit(DecodeSpec spec, Done done, Explicit kind = Explicit::OneShot);
    void captured(const Interest &interest, std::variant<CapturedSource, ProviderSnapshot> input);
    void setVisible(const Interest &interest, bool visible);
    Asset providerAsset(QImage pixels, QSize original, DecodeSpec spec);
    quint64 submissions() const { return m_submissions; }

private:
    enum Phase { Capturing, Waiting, Preparing, LookingUp, Ready, Decoding, Finishing };
    struct Pending {
        std::variant<PreparationInput, OpenedSource, ProviderSnapshot> source;
        QByteArray key;
        std::shared_ptr<Fd> input;
        DecodeSpec spec;
        QDeadlineTimer deadline;
        Phase phase = Waiting;
        quint64 order = 0;
        int ticket = 0;
        bool oneShot = false;
        std::shared_ptr<void> preparing;
        QList<std::weak_ptr<Listener>> listeners;
        bool classified = false;
        ImageStore::Lookup cached;
        std::optional<struct stat> file;
    };
    Interest listen(Done done, bool visible);
    std::shared_ptr<Pending> enqueue(const Interest &interest, DecodeSpec spec, Phase phase);
    void opened(const std::shared_ptr<Pending> &request, OpenedSource source);
    void remove(const std::shared_ptr<Pending> &request);
    void finish(const std::shared_ptr<Pending> &request, Decoded decoded, bool hit = false,
                Asset asset = {});
    void deliver(std::weak_ptr<Listener> listener, Asset asset, Failure error);
    void submit(const std::shared_ptr<Pending> &request);
    void pumpLater();
    void pump();
    Asset exchangeAsset(ImageAsset asset);
    void prune();

    Pool &m_pool;
    int m_waitMs;
    QTimer m_deadline;
    QHash<QByteArray, std::shared_ptr<Pending>> m_pending;
    QList<std::shared_ptr<Pending>> m_queue;
    QHash<QByteArray, std::weak_ptr<const ImageAsset>> m_assets;
    bool m_scheduled = false;
    quint64 m_order = 0;
    quint64 m_submissions = 0;
};

ImagePipeline &imagePipeline();

} // namespace Imv
