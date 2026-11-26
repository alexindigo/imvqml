// SPDX-License-Identifier: LGPL-3.0-only
#include "image-pipeline.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <fcntl.h>

namespace Imv {

ImagePipeline::ImagePipeline(Pool &pool, QObject *parent) : QObject(parent), m_pool(pool) {
    connect(&pool, &Pool::capacityReleased, this, &ImagePipeline::pumpLater);
}

ImagePipeline::Interest ImagePipeline::interest(Done done,
                                                const std::shared_ptr<Pending> &pending) {
    return Interest(new Done(std::move(done)),
                    [self = QPointer(this), weak = std::weak_ptr(pending)](const Done *callback) {
                        delete callback;
                        auto request = weak.lock();
                        if (!self || !request || self->m_pending.value(request->key) != request)
                            return;
                        request->listeners.removeIf(
                            [](const auto &listener) { return listener.expired(); });
                        if (!request->listeners.isEmpty())
                            return;
                        self->m_pending.remove(request->key);
                        if (request->ticket)
                            self->m_pool.cancel(request->ticket);
                    });
}

ImagePipeline::Interest ImagePipeline::request(OpenedSource source, DecodeSpec spec, Done done) {
    const auto key = source.identity + '\0' + encode(spec);
    if (!source.error && !spec.valid())
        source.error = {"admission", "invalid-size", "Physical size exceeds the output edge limit"};
    prune();
    auto asset = m_assets.value(key).lock();
    if (source.error || asset) {
        auto lease = interest(std::move(done));
        QMetaObject::invokeMethod(
            this,
            [weak = std::weak_ptr(lease), asset = std::move(asset),
             error = std::move(source.error)] {
                if (const auto callback = weak.lock())
                    (*callback)(asset, error);
            },
            Qt::QueuedConnection);
        return lease;
    }
    auto &entry = m_pending[key];
    if (!entry) {
        entry = std::make_shared<Pending>(Pending{key, std::move(source.fd), spec, 0, {}});
        m_waiting.append(entry);
        pumpLater();
    }
    auto lease = interest(std::move(done), entry);
    entry->listeners.append(lease);
    return lease;
}

void ImagePipeline::pumpLater() {
    if (std::exchange(m_scheduled, true))
        return;
    QMetaObject::invokeMethod(this, &ImagePipeline::pump, Qt::QueuedConnection);
}

void ImagePipeline::pump() {
    m_scheduled = false;
    while (!m_waiting.isEmpty() && m_pool.available()) {
        const auto request = m_waiting.takeFirst().lock();
        if (!request || m_pending.value(request->key) != request)
            continue;
        const auto ticket = submitDecode(m_pool, std::move(request->input), request->spec,
                                         [self = QPointer(this), request](Decoded decoded) {
                                             if (self)
                                                 self->finish(request, std::move(decoded));
                                         });
        if (ticket.status == Pool::Admission::Accepted) {
            request->ticket = ticket.id;
            ++m_submissions;
        } else {
            finish(request,
                   {{}, {}, {"admission", "output-failed", "Could not admit a display decode"}});
        }
    }
    // Waiting entries whose owners disappeared must not accumulate while the pool is occupied.
    m_waiting.removeIf([](const auto &weak) { return weak.expired(); });
}

void ImagePipeline::finish(const std::shared_ptr<Pending> &request, Decoded decoded) {
    if (m_pending.value(request->key) != request)
        return;
    m_pending.remove(request->key);
    Asset asset;
    if (!decoded.error)
        asset = exchangeAsset({request->key, std::move(decoded.source), std::move(decoded.pixels)});
    for (const auto &weak : std::as_const(request->listeners)) {
        if (const auto callback = weak.lock())
            (*callback)(asset, decoded.error);
    }
}

void ImagePipeline::prune() {
    m_assets.removeIf([](const auto &entry) { return entry.value().expired(); });
}

Asset ImagePipeline::exchangeAsset(ImageAsset asset) {
    auto &weak = m_assets[asset.key];
    auto existing = weak.lock();
    if (existing)
        return existing;
    existing = std::make_shared<const ImageAsset>(std::move(asset));
    weak = existing;
    return existing;
}

Asset ImagePipeline::providerAsset(QImage pixels, QSize original, DecodeSpec spec) {
    prune();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(pixels.width()) + 'x' + QByteArray::number(pixels.height()));
    for (int y = 0; y < pixels.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char *>(pixels.constScanLine(y)),
                                    pixels.width() * 4));
    const auto key = "provider/" + hash.result() + encode(spec);
    // Equal pixels can carry different original dimensions in different providers.
    return exchangeAsset(
        {key + QByteArray::number(original.width()) + 'x' + QByteArray::number(original.height()),
         {"provider", original, 1, 1},
         std::move(pixels)});
}

ImagePipeline &imagePipeline() {
    static QPointer<ImagePipeline> pipeline;
    if (!pipeline)
        pipeline = new ImagePipeline(decoderPool(), QCoreApplication::instance());
    return *pipeline;
}
} // namespace Imv
