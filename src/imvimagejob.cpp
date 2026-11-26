// SPDX-License-Identifier: LGPL-3.0-only
#include "imvimagejob.h"
#include "private/decode-client.h"
#include "private/export.h"

#include <QDir>
#include <QHash>
#include <QPointer>
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
        if (!spec.valid()) {
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
        if (!pool)
            pool = &decoderPool();
        if (!argumentError && !pool->available())
            return -2;
        auto source =
            argumentError ? OpenedSource{{}, {}, std::move(argumentError)} : openSource(url);
        auto job = std::shared_ptr<Active>(
            new Active{++lastId, 0, method, std::move(path), std::move(format)});
        const auto failLater = [this, &job](Failure error) {
            jobs.insert(job->id, job);
            QMetaObject::invokeMethod(
                &q, [this, job, error = std::move(error)] { finish(job, asMap(error)); },
                Qt::QueuedConnection);
        };
        if (source.error) {
            failLater(std::move(source.error));
            return job->id;
        }
        const QPointer<ImvImageJob> receiver(&q);
        const auto ticket =
            submitDecode(*pool, std::move(source.fd), spec, [receiver, job](Decoded image) mutable {
                if (receiver)
                    receiver->d->decoded(job, std::move(image));
            });
        if (ticket.status == Pool::Admission::Busy)
            return -2;
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
    return true;
}

} // namespace Imv
