// SPDX-License-Identifier: LGPL-3.0-only
#include "image-pipeline.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <algorithm>

namespace Imv {
namespace {
Failure expired() { return {"admission", "deadline", "Image admission deadline expired"}; }
} // namespace

ImagePipeline::ImagePipeline(Pool &pool, QObject *parent, int waitMs)
    : QObject(parent), m_pool(pool), m_waitMs(waitMs) {
    connect(&pool, &Pool::capacityReleased, this, &ImagePipeline::pumpLater);
    onSourceCapacity(this, [this] { pumpLater(); });
    m_deadline.setSingleShot(true);
    m_deadline.setTimerType(Qt::PreciseTimer);
    connect(&m_deadline, &QTimer::timeout, this, &ImagePipeline::pumpLater);
}

ImagePipeline::Interest ImagePipeline::request(std::variant<SourceInput, OpenedSource> source,
                                               DecodeSpec spec, Done done, bool visible) {
    auto lease = listen(std::move(done), visible);
    if (!spec.valid()) {
        deliver(lease, {},
                {"admission", "invalid-size", "Physical size exceeds the output edge limit"});
    } else if (m_queue.size() >= QueueLimit) {
        deliver(lease, {}, {"admission", "deadline", "Image admission queue is full"});
    } else {
        auto request = enqueue(lease, spec, Waiting);
        std::visit([&](auto &&input) { request->source = std::move(input); }, std::move(source));
    }
    return lease;
}

ImagePipeline::Interest ImagePipeline::requestExplicit(DecodeSpec spec, Done done, Explicit kind) {
    if (m_queue.size() >= QueueLimit)
        return {};
    auto lease = listen(std::move(done), false);
    enqueue(lease, spec, Capturing)->kind = kind;
    return lease;
}

void ImagePipeline::captured(const Interest &interest,
                             std::variant<CapturedSource, ProviderSnapshot> input) {
    auto request = interest ? interest->pending.lock() : nullptr;
    if (!request || request->phase != Capturing || !m_queue.contains(request))
        return;
    if (request->deadline.hasExpired()) {
        finish(request, {{}, {}, expired()});
        return;
    }
    std::visit(
        [&](auto &&captured) {
            if (captured.error) {
                finish(request, {{}, {}, std::move(captured.error)});
            } else {
                request->source = std::move(captured);
                request->phase = Waiting;
                pumpLater();
            }
        },
        std::move(input));
}

ImagePipeline::Interest ImagePipeline::listen(Done done, bool visible) {
    Interest lease(new Listener, [self = QPointer(this)](Listener *listener) {
        auto request = listener->pending.lock();
        delete listener;
        if (!self || !request)
            return;
        request->listeners.removeIf([](const auto &weak) { return weak.expired(); });
        if (request->listeners.isEmpty())
            self->remove(request);
    });
    lease->done = std::move(done);
    lease->visible = visible;
    return lease;
}

std::shared_ptr<ImagePipeline::Pending> ImagePipeline::enqueue(const Interest &interest,
                                                               DecodeSpec spec, Phase phase) {
    auto request = std::make_shared<Pending>();
    request->spec = spec;
    request->phase = phase;
    request->deadline = QDeadlineTimer(m_waitMs, Qt::PreciseTimer);
    request->order = ++m_order;
    request->listeners.append(interest);
    interest->pending = request;
    m_queue.append(request);
    pumpLater();
    return request;
}

void ImagePipeline::setVisible(const Interest &interest, bool visible) {
    if (interest && std::exchange(interest->visible, visible) != visible)
        pumpLater();
}

void ImagePipeline::opened(const std::shared_ptr<Pending> &request, OpenedSource source) {
    request->preparing.reset();
    request->source = PreparationInput{};
    if (request->deadline.hasExpired()) {
        finish(request, {{}, {}, expired()});
        return;
    }
    if (source.error) {
        finish(request, {{}, {}, std::move(source.error)});
        return;
    }
    if (request->oneShot()) {
        request->input = std::make_shared<Fd>(std::move(source.fd));
        request->file = source.file;
        request->phase = Ready;
        return;
    }
    auto &store = imageStore();
    request->key =
        source.identity + '\0' + encode(request->spec) + QByteArray::number(store.epoch());
    prune();
    if (auto asset = m_assets.value(request->key).lock()) {
        finish(request, {}, true, std::move(asset));
        return;
    }
    auto other = m_pending.value(request->key);
    if (other && other->phase != Decoding && other->deadline.hasExpired()) {
        finish(other, {{}, {}, expired()});
        other.reset();
    }
    if (other) {
        other->order = qMin(other->order, request->order);
        for (const auto &weak : std::as_const(request->listeners)) {
            if (auto listener = weak.lock()) {
                listener->pending = other;
                other->listeners.append(weak);
            }
        }
        remove(request);
        return;
    }
    m_pending.insert(request->key, request);
    request->input = std::make_shared<Fd>(std::move(source.fd));
    request->file = source.file;
    request->phase = Ready;
    if (!store.enabled())
        return;
    request->phase = LookingUp;
    store.lookup(std::weak_ptr<const Fd>(request->input), request->spec,
                 [self = QPointer(this), weak = std::weak_ptr(request)](ImageStore::Lookup result) {
                     const auto active = weak.lock();
                     if (!self || !active || !self->m_queue.contains(active))
                         return;
                     if (active->deadline.hasExpired()) {
                         self->finish(active, {{}, {}, expired()});
                         return;
                     }
                     auto value = std::exchange(result.value, {});
                     active->cached = std::move(result);
                     if (value) {
                         self->finish(active, std::move(*value), true);
                     } else {
                         active->phase = Ready;
                         self->pumpLater();
                     }
                 });
}

void ImagePipeline::pumpLater() {
    if (!std::exchange(m_scheduled, true))
        QMetaObject::invokeMethod(this, &ImagePipeline::pump, Qt::QueuedConnection);
}

void ImagePipeline::pump() {
    // Start preparation only against an idle helper slot. This leaves queued
    // sources eligible for visibility reordering until a slot is available,
    // keeps sealed snapshots bounded, and avoids pre-staging hidden work.
    constexpr int StagedLimit = 4;
    for (const auto &request : QList(m_queue)) {
        if (request->phase != Decoding && request->phase != Finishing &&
            request->deadline.hasExpired())
            finish(request, {{}, {}, expired()});
    }
    bool sourceBusy = false;
    for (;;) {
        const auto reservations = std::ranges::count_if(m_queue, [](const auto &request) {
            return request->phase == Preparing || request->phase == LookingUp ||
                   request->phase == Ready;
        });
        // A one-shot job retains its slot through publication, so payloads
        // cannot accumulate in the render encoder's worker queue.
        const auto activeJobs = std::ranges::count_if(m_queue, [](const auto &request) {
            return request->oneShot() &&
                   (request->phase == Decoding || request->phase == Finishing);
        });
        const auto capacity = m_pool.capacity();
        const bool canStage = reservations < qMin(m_pool.idleSlots(), StagedLimit) ||
                              (capacity == Pool::Admission::Exhausted && !reservations);
        const bool canDecode = capacity != Pool::Admission::Busy;
        std::shared_ptr<Pending> next;
        bool nextVisible = false;
        for (const auto &candidate : std::as_const(m_queue)) {
            const bool alreadyOpen = std::holds_alternative<OpenedSource>(candidate->source);
            const bool provider = std::holds_alternative<ProviderSnapshot>(candidate->source);
            if (candidate->oneShot() && activeJobs >= 2)
                continue;
            if (!(candidate->phase == Ready && canDecode) &&
                !(candidate->phase == Waiting &&
                  (alreadyOpen ? reservations < StagedLimit
                   : provider  ? (canDecode || candidate->kind == Explicit::RetainOriginal)
                               : canStage && !sourceBusy)))
                continue;
            const bool visible = std::ranges::any_of(candidate->listeners, [](const auto &weak) {
                const auto listener = weak.lock();
                return listener && listener->visible;
            });
            if (!next || visible > nextVisible ||
                (visible == nextVisible && candidate->order < next->order)) {
                next = candidate;
                nextVisible = visible;
            }
        }
        if (!next)
            break;
        if (next->deadline.hasExpired()) {
            finish(next, {{}, {}, expired()});
        } else if (next->phase == Ready) {
            submit(next);
        } else if (auto *provider = std::get_if<ProviderSnapshot>(&next->source)) {
            // A provider has pixels but no encoded bytes. Export its owned
            // native snapshot without fitting it or asking the provider again.
            auto decoded = next->kind == Explicit::RetainOriginal
                               ? Decoded{{"png", provider->original}, std::move(provider->pixels)}
                               : rasterizeProvider(*provider, next->spec);
            next->source = PreparationInput{};
            finish(next, std::move(decoded));
        } else if (auto *input = std::get_if<OpenedSource>(&next->source)) {
            // Consume the variant before opened() clears it or changes phase.
            auto source = std::move(*input);
            opened(next, std::move(source));
        } else {
            next->preparing = prepareSource(
                std::get<PreparationInput>(next->source),
                [self = QPointer(this), weak = std::weak_ptr(next)](OpenedSource source) {
                    if (const auto active = weak.lock();
                        self && active && self->m_queue.contains(active)) {
                        self->opened(active, std::move(source));
                        self->pumpLater();
                    }
                });
            if (next->preparing) {
                next->phase = Preparing;
                next->source = PreparationInput{};
            } else {
                sourceBusy = true;
            }
        }
    }
    qint64 remaining = m_waitMs;
    bool hasDeadline = false;
    for (const auto &request : std::as_const(m_queue)) {
        if (request->phase != Decoding && request->phase != Finishing) {
            hasDeadline = true;
            remaining = qMin(remaining, request->deadline.remainingTime());
        }
    }
    if (hasDeadline)
        m_deadline.start(int(qMax<qint64>(1, remaining)));
    else
        m_deadline.stop();
    m_scheduled = false;
}

void ImagePipeline::submit(const std::shared_ptr<Pending> &request) {
    const bool classify = request->spec.mode == DecodeSpec::Animation && !request->classified;
    const auto ticket = submitDecode(
        m_pool, duplicateSource(*request->input), classify ? DecodeSpec{} : request->spec,
        [self = QPointer(this), weak = std::weak_ptr(request), classify](Decoded decoded) {
            const auto active = weak.lock();
            if (!self || !active || !self->m_queue.contains(active))
                return;
            active->ticket = 0;
            if (classify && !decoded.error) {
                active->classified = true;
                if (!decoded.delay)
                    active->spec.mode = DecodeSpec::Pixels;
                active->phase = Ready;
                active->deadline = QDeadlineTimer(self->m_waitMs, Qt::PreciseTimer);
                self->pumpLater();
            } else {
                self->finish(active, std::move(decoded));
            }
        });
    if (ticket.status == Pool::Admission::Busy)
        return;
    if (ticket.status != Pool::Admission::Accepted) {
        finish(request,
               {{}, {}, {"admission", "output-failed", "Could not admit an image decode"}});
        return;
    }
    request->phase = Decoding;
    request->ticket = ticket.id;
    if (!classify)
        ++m_submissions;
}

void ImagePipeline::remove(const std::shared_ptr<Pending> &request) {
    if (!m_queue.removeOne(request))
        return;
    if (m_pending.value(request->key) == request)
        m_pending.remove(request->key);
    request->preparing.reset();
    if (request->ticket)
        m_pool.cancel(request->ticket);
    pumpLater();
}

void ImagePipeline::deliver(std::weak_ptr<Listener> listener, Asset asset, Failure error) {
    QMetaObject::invokeMethod(
        this,
        [listener = std::move(listener), asset = std::move(asset), error = std::move(error)] {
            if (const auto callback = listener.lock())
                callback->done(asset, error);
        },
        Qt::QueuedConnection);
}

void ImagePipeline::finish(const std::shared_ptr<Pending> &request, Decoded decoded, bool hit,
                           Asset asset) {
    if (request->oneShot() && !decoded.error) {
        request->phase = Finishing;
        ImageAsset finished{{}, std::move(decoded.source), std::move(decoded.pixels)};
        if (request->kind == Explicit::RetainOriginal)
            finished.original = request->input;
        request->input.reset();
        asset = std::make_shared<const ImageAsset>(std::move(finished));
        for (const auto &weak : std::as_const(request->listeners))
            deliver(weak, asset, {});
        return;
    }
    remove(request);
    if (!hit && request->cached.lease)
        imageStore().publish(request->cached, decoded);
    if (!asset && !decoded.error) {
        auto stream = decoded.stream ? request->input : nullptr;
        asset = exchangeAsset({request->key, std::move(decoded.source), std::move(decoded.pixels),
                               std::move(decoded.animation), std::move(stream), request->spec,
                               decoded.delay, false, request->cached.lease, request->file});
    }
    for (const auto &weak : std::as_const(request->listeners))
        deliver(weak, asset, decoded.error);
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
