// SPDX-License-Identifier: LGPL-3.0-only
#include "imvimagejob.h"
#include "private/export.h"
#include "private/image-pipeline.h"

#include <QDir>
#include <QHash>
#include <QPointer>
#include <QQmlEngine>
#include <QThreadPool>
#include <limits>
#include <mutex>

namespace Imv {
struct ImvImageJob::State {
    enum Method { Inspect, Sample, Render, RetainOriginal };
    struct Dispatch {
        std::mutex lock;
        ImvImageJob *target;
    };
    struct Active {
        int id;
        Method method;
        QString destination, format;
        std::atomic<Publication> phase{Publication::Staging};
        std::function<void()> cancelProvider;
        bool writing = false;
        ImagePipeline::Interest interest;
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
            job->interest.reset();
            if (auto cancel = std::exchange(job->cancelProvider, {}))
                cancel();
        }
    }

    int begin(Method method, const QUrl &url, QSize bounds, const QUrl &dest = {},
              QString format = {}, double position = 0) {
        if (lastId == std::numeric_limits<int>::max())
            return -3;
        const bool retaining = method == RetainOriginal;
        const DecodeSpec spec{method == Inspect || retaining ? DecodeSpec::Inspect
                                                             : DecodeSpec::Pixels,
                              bounds, DecodeSpec::Contain, 0, position};
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
        } else if (retaining) {
            path = localPath(dest);
            if (path.isEmpty())
                argumentError = {"admission", "invalid-destination",
                                 "Retain requires a local directory"};
            else
                path = QDir::current().absoluteFilePath(path);
        }
        auto job = std::shared_ptr<Active>(
            new Active{lastId + 1, method, std::move(path), std::move(format)});
        if (argumentError) {
            ++lastId;
            jobs.insert(job->id, job);
            finishLater(job, asMap(argumentError));
            return job->id;
        }

        const QPointer<ImvImageJob> receiver(&q);
        auto &pipeline = imagePipeline();
        job->interest = pipeline.requestExplicit(
            spec,
            [receiver, weak = std::weak_ptr(job)](Asset asset, Failure error) {
                if (auto active = weak.lock(); receiver && active)
                    receiver->d->decoded(active, std::move(asset), std::move(error));
            },
            retaining ? ImagePipeline::Explicit::RetainOriginal : ImagePipeline::Explicit::OneShot);
        if (!job->interest)
            return -2;
        ++lastId;
        jobs.insert(job->id, job);
        if (url.scheme() == "image") {
            auto cancel = snapshotProvider(
                qmlEngine(&q), url, &q,
                [receiver, weak = std::weak_ptr(job)](ProviderSnapshot image) {
                    if (const auto active = weak.lock(); receiver && active)
                        imagePipeline().captured(active->interest, std::move(image));
                });
            if (receiver && job->interest)
                job->cancelProvider = std::move(cancel);
            else if (cancel)
                cancel();
        } else {
            pipeline.captured(job->interest, captureSource(url, retaining ? SourceLimit::Strict
                                                                          : SourceLimit::Decode));
        }
        return job->id;
    }

    void finishLater(const std::shared_ptr<Active> &job, QVariantMap result = {}) {
        QMetaObject::invokeMethod(
            &q,
            [this, job, result = std::move(result)]() mutable { finish(job, std::move(result)); },
            Qt::QueuedConnection);
    }

    void finish(const std::shared_ptr<Active> &job, QVariantMap result) {
        if (!jobs.remove(job->id))
            return;
        if (job->phase.load() == Publication::Cancelled)
            result = asMap(Failure{"helper", "cancelled", "Image job cancelled"});
        job->interest.reset();
        const QPointer receiver(&q);
        if (auto cancel = std::exchange(job->cancelProvider, {}))
            cancel();
        if (receiver)
            emit receiver->finished(job->id, result);
    }

    void decoded(const std::shared_ptr<Active> &job, Asset image, Failure error) {
        if (job->phase.load() == Publication::Cancelled || error) {
            finish(job, asMap(error));
            return;
        }
        QVariantMap result{{"stage", "complete"},
                           {"code", QString{}},
                           {"detail", QString{}},
                           {"sourceInfo", asMap(image->info)}};
        if (job->method == Inspect) {
            finish(job, std::move(result));
            return;
        }
        if (job->method == RetainOriginal) {
            result.remove("sourceInfo");
            write(job, std::move(image), std::move(result));
            return;
        }
        result.insert("width", image->pixels.width());
        result.insert("height", image->pixels.height());
        if (job->method == Sample) {
            result.insert("rgba",
                          QByteArray(reinterpret_cast<const char *>(image->pixels.constBits()),
                                     image->pixels.sizeInBytes()));
            finish(job, std::move(result));
            return;
        }
        write(job, std::move(image), std::move(result));
    }

    void write(const std::shared_ptr<Active> &job, Asset image, QVariantMap result) {
        job->writing = true;
        QThreadPool::globalInstance()->start([dispatch = dispatch, job, image = std::move(image),
                                              result = std::move(result)]() mutable {
            Failure error;
            if (job->method == RetainOriginal) {
                auto retained = stageOriginal(job->destination, image->original.get(),
                                              image->pixels, job->phase);
                error = std::move(retained.error);
                result.insert("path", std::move(retained.path));
                result.insert("size", retained.size);
                result.insert("sha256", std::move(retained.sha256));
                result.insert("format", image->info.format);
            } else {
                error = stageImage(job->destination, job->format, image->pixels, job->phase);
            }
            image.reset();
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
int ImvImageJob::retainOriginal(const QUrl &source, const QUrl &destinationDirectory) {
    return d->begin(State::RetainOriginal, source, QSize(0, 0), destinationDirectory);
}
bool ImvImageJob::cancel(int id) {
    auto job = d->jobs.value(id);
    auto expected = Publication::Staging;
    if (!job || !job->phase.compare_exchange_strong(expected, Publication::Cancelled))
        return false;
    if (!job->writing) {
        job->interest.reset();
        d->finishLater(job);
        if (auto cancel = std::exchange(job->cancelProvider, {}))
            cancel();
    }
    return true;
}

} // namespace Imv
