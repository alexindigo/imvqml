// SPDX-License-Identifier: LGPL-3.0-only
#include "imvimagejob.h"
#include "private/decode-client.h"
#include "private/export.h"
#include "private/image-provider.h"

#include <QDir>
#include <QHash>
#include <QPointer>
#include <QQmlEngine>
#include <QThreadPool>
#include <limits>
#include <mutex>

namespace Imv {

struct ImvImageJob::State {
    enum Method { Inspect, Sample, Render };
    struct Dispatch {
        std::mutex lock;
        ImvImageJob *target;
    };
    struct Active {
        int id;
        int poolId = 0;
        Method method;
        QString destination, format;
        std::atomic<Publication> phase{Publication::Staging};
        std::function<void()> cancelProvider;
        bool awaitingProvider = false;
    };

    explicit State(ImvImageJob &owner) : q(owner), dispatch(std::make_shared<Dispatch>()) {
        dispatch->target = &q;
    }

    ~State() {
        {
            const std::lock_guard lock(dispatch->lock);
            dispatch->target = nullptr;
        }
        for (const auto &job : std::as_const(jobs)) {
            auto expected = Publication::Staging;
            job->phase.compare_exchange_strong(expected, Publication::Cancelled);
            if (job->poolId > 0 && pool)
                pool->cancel(job->poolId);
            if (auto cancel = std::exchange(job->cancelProvider, {}))
                cancel();
        }
    }

    int begin(Method method, const QUrl &url, QSize bounds, const QUrl &dest = {},
              QString format = {}, double position = 0) {
        if (lastId == std::numeric_limits<int>::max())
            return -3;
        const DecodeSpec spec{method == Inspect ? DecodeSpec::Inspect : DecodeSpec::Pixels, bounds,
                              DecodeSpec::Contain, 0, position};
        QString path;
        Failure argumentError;
        if (!DecodeSpec::validPosition(position)) {
            argumentError = {"admission", "invalid-position",
                             "Video position must be finite and between 0 and 1"};
        } else if (!spec.valid()) {
            argumentError = {"admission", "invalid-size",
                             "Output bounds must be from 1 to 8192 pixels"};
        } else if (method == Render) {
            path = localPath(dest);
            if (path.isEmpty() || path.endsWith('/') || path.endsWith("/.") ||
                path.endsWith("/..") || path == "." || path == ".." ||
                (format != "png" && format != "webp")) {
                argumentError = {"admission", "invalid-destination",
                                 "Render requires an unused local file and png or webp"};
            } else {
                path = QDir::current().absoluteFilePath(path);
            }
        }
        const bool fromProvider = !argumentError && url.scheme() == "image";
        if (!argumentError && !fromProvider) {
            if (!pool)
                pool = &decoderPool();
            if (pool->capacity() == Pool::Admission::Exhausted)
                return -3;
            if (pool->capacity() == Pool::Admission::Busy)
                return -2;
        }
        auto job = std::shared_ptr<Active>(
            new Active{++lastId, 0, method, std::move(path), std::move(format)});
        const auto failLater = [this, &job](Failure error) {
            jobs.insert(job->id, job);
            QMetaObject::invokeMethod(
                &q, [this, job, error = std::move(error)] { finish(job, asMap(error)); },
                Qt::QueuedConnection);
        };
        if (argumentError) {
            failLater(std::move(argumentError));
            return job->id;
        }
        const QPointer<ImvImageJob> receiver(&q);
        if (fromProvider) {
            job->awaitingProvider = true;
            jobs.insert(job->id, job);
            const auto captured = [receiver, weak = std::weak_ptr(job),
                                   spec](ProviderSnapshot snapshot) {
                const auto active = weak.lock();
                if (!receiver || !active)
                    return;
                QMetaObject::invokeMethod(
                    receiver,
                    [receiver, active, spec, snapshot = std::move(snapshot)] {
                        if (receiver && active->phase.load() != Publication::Cancelled)
                            receiver->d->decoded(active, rasterizeProvider(snapshot, spec));
                    },
                    Qt::QueuedConnection);
            };
            auto cancel = snapshotProvider(qmlEngine(&q), url, &q, captured);
            if (receiver && job->phase.load() != Publication::Cancelled)
                job->cancelProvider = std::move(cancel);
            else if (cancel)
                cancel();
            return job->id;
        }
        auto source = openSource(url);
        if (source.error) {
            failLater(std::move(source.error));
            return job->id;
        }
        const auto ticket =
            submitDecode(*pool, std::move(source.fd), spec, [receiver, job](Decoded image) mutable {
                if (receiver)
                    receiver->d->decoded(job, std::move(image));
            });
        if (ticket.status == Pool::Admission::Busy)
            return -2;
        if (ticket.status == Pool::Admission::Exhausted)
            return -3;
        if (ticket.status != Pool::Admission::Accepted) {
            failLater({"admission", "output-failed", "Cannot allocate decode output or ticket"});
            return job->id;
        }
        job->poolId = ticket.id;
        jobs.insert(job->id, job);
        return job->id;
    }

    void finish(const std::shared_ptr<Active> &job, QVariantMap result) {
        if (!jobs.remove(job->id))
            return;
        if (job->phase.load() == Publication::Cancelled)
            result = asMap(Failure{"helper", "cancelled", "Image job cancelled"});
        emit q.finished(job->id, result);
    }

    void decoded(const std::shared_ptr<Active> &job, Decoded image) {
        job->poolId = 0;
        job->awaitingProvider = false;
        job->cancelProvider = {};
        if (job->phase.load() == Publication::Cancelled || image.error) {
            finish(job, asMap(image.error));
            return;
        }
        QVariantMap result{{"stage", "complete"},
                           {"code", QString{}},
                           {"detail", QString{}},
                           {"sourceInfo", asMap(image.source)}};
        if (job->method == Inspect) {
            finish(job, std::move(result));
            return;
        }
        result.insert("width", image.pixels.width());
        result.insert("height", image.pixels.height());
        if (job->method == Sample) {
            result.insert("rgba",
                          QByteArray(reinterpret_cast<const char *>(image.pixels.constBits()),
                                     image.pixels.sizeInBytes()));
            finish(job, std::move(result));
            return;
        }
        QThreadPool::globalInstance()->start([dispatch = dispatch, job,
                                              image = std::move(image.pixels),
                                              result = std::move(result)]() mutable {
            auto error = stageImage(job->destination, job->format, image, job->phase);
            if (error)
                result = asMap(error);
            const std::lock_guard lock(dispatch->lock);
            if (auto *target = dispatch->target) {
                QMetaObject::invokeMethod(
                    target,
                    [target, job, result = std::move(result)]() mutable {
                        target->d->finish(job, std::move(result));
                    },
                    Qt::QueuedConnection);
            }
        });
    }

    ImvImageJob &q;
    QPointer<Pool> pool;
    std::shared_ptr<Dispatch> dispatch;
    QHash<int, std::shared_ptr<Active>> jobs;
    int lastId = 0;
};

ImvImageJob::ImvImageJob(QObject *parent) : QObject(parent), d(std::make_unique<State>(*this)) {}
ImvImageJob::~ImvImageJob() = default;

int ImvImageJob::inspect(const QUrl &source) {
    return d->begin(State::Inspect, source, QSize(0, 0));
}
int ImvImageJob::sample(const QUrl &source, int width, int height, double position) {
    return d->begin(State::Sample, source, QSize(width, height), {}, {}, position);
}
int ImvImageJob::render(const QUrl &source, int width, int height, const QUrl &destination,
                        const QString &format, double position) {
    return d->begin(State::Render, source, QSize(width, height), destination, format, position);
}
bool ImvImageJob::cancel(int id) {
    auto job = d->jobs.value(id);
    auto expected = Publication::Staging;
    if (!job || !job->phase.compare_exchange_strong(expected, Publication::Cancelled))
        return false;
    if (job->poolId > 0 && d->pool)
        d->pool->cancel(job->poolId);
    else if (job->awaitingProvider) {
        const QPointer guard(this);
        if (auto cancel = std::exchange(job->cancelProvider, {}))
            cancel();
        if (guard)
            QMetaObject::invokeMethod(
                this, [this, job] { d->finish(job, {}); }, Qt::QueuedConnection);
    }
    return true;
}

} // namespace Imv
