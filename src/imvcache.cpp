// SPDX-License-Identifier: LGPL-3.0-only
#include "imvcache.h"
#include "private/image-pipeline.h"

#include <QDir>
#include <QHash>
#include <QPointer>
#include <QStandardPaths>

namespace Imv {
namespace {
struct Policy {
    QString directory =
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/imvqml";
    qint64 budget = ImageStore::DefaultBudget;
    bool enabled = false;
    void apply() { imageStore().configure(directory, budget, enabled); }
};

class PolicyOwner : public QObject {
    Q_OBJECT
public:
    Policy value;
signals:
    void changed();
};

PolicyOwner &policy() {
    static PolicyOwner shared;
    return shared;
}
} // namespace

struct ImvCache::State {
    int lastId = 0;
    QHash<int, ImagePipeline::Interest> exports;
};

ImvCache::ImvCache(QObject *parent) : QObject(parent), d(std::make_unique<State>()) {
    connect(&policy(), &PolicyOwner::changed, this, &ImvCache::policyChanged);
}

ImvCache::~ImvCache() = default;

QUrl ImvCache::directory() const { return QUrl::fromLocalFile(policy().value.directory); }
void ImvCache::setDirectory(const QUrl &directory) {
    auto path = localPath(directory);
    if (!QDir::isAbsolutePath(path))
        return;
    path = QDir::cleanPath(path);
    if (policy().value.directory == path)
        return;
    policy().value.directory = std::move(path);
    policy().value.apply();
    emit policy().changed();
}

qint64 ImvCache::byteBudget() const { return policy().value.budget; }
void ImvCache::setByteBudget(qint64 bytes) {
    if (bytes < 0 || policy().value.budget == bytes)
        return;
    policy().value.budget = bytes;
    policy().value.apply();
    emit policy().changed();
}

bool ImvCache::enabled() const { return policy().value.enabled; }
void ImvCache::setEnabled(bool enabled) {
    if (policy().value.enabled == enabled)
        return;
    policy().value.enabled = enabled;
    policy().value.apply();
    emit policy().changed();
}

int ImvCache::clear() { return evict(nullptr); }

int ImvCache::invalidate(const QUrl &url) { return evict(&url); }

int ImvCache::evict(const QUrl *source) {
    if (d->lastId == INT_MAX)
        return -3;
    const int ticket = ++d->lastId;
    // Also handle a store that has never been enabled in this process.
    policy().value.apply();
    const auto done = [self = QPointer(this), ticket](ImageStore::Eviction eviction) {
        if (!self)
            return;
        auto result = asMap(eviction.error ? eviction.error : Failure{"complete", {}, {}});
        result.insert("removed", eviction.removed);
        result.insert("kept", eviction.kept);
        result.insert("failed", eviction.failed);
        emit self->cleared(ticket, result);
    };
    if (source)
        imageStore().invalidate(*source, done);
    else
        imageStore().clear(done);
    return ticket;
}

int ImvCache::exportThumbnail(const QUrl &source, int size) {
    if (d->lastId == INT_MAX)
        return -3;
    const int id = ++d->lastId;
    auto path = localPath(source);
    if (path.isEmpty() || thumbnailBucket(size).isEmpty()) {
        QMetaObject::invokeMethod(
            this,
            [this, id] {
                finish(id, asMap(Failure{
                               "admission", "invalid-source",
                               "Thumbnail requires a local file and a standard bucket size"}));
            },
            Qt::QueuedConnection);
        return id;
    }
    path = QDir::cleanPath(QDir::current().absoluteFilePath(path));
    auto &pipeline = imagePipeline();
    auto interest = pipeline.requestExplicit(
        {DecodeSpec::Pixels, {size, size}},
        [self = QPointer(this), id, path, size](Asset asset, Failure error) {
            if (!self)
                return;
            if (error || !asset->file) {
                self->finish(
                    id, asMap(error ? error
                                    : Failure{"open", "unreadable", "Missing source revision"}));
                return;
            }
            imageStore().thumbnail(
                path, *asset->file, asset->pixels, asset->info, size, self->d->exports.value(id),
                [self, id](Failure failure, QUrl output) {
                    if (!self)
                        return;
                    auto result = failure ? asMap(failure) : asMap(Failure{"complete", {}, {}});
                    if (!failure)
                        result.insert("destination", output);
                    self->finish(id, result);
                });
        },
        ImagePipeline::Explicit::Thumbnail);
    if (!interest) {
        QMetaObject::invokeMethod(
            this,
            [this, id] {
                finish(id,
                       asMap(Failure{"admission", "deadline", "Image admission queue is full"}));
            },
            Qt::QueuedConnection);
    } else {
        d->exports.insert(id, interest);
        std::visit([&](auto &&input) { pipeline.captured(interest, std::move(input)); },
                   captureSource(QUrl::fromLocalFile(path)));
    }
    return id;
}

void ImvCache::finish(int id, const QVariantMap &result) {
    d->exports.remove(id);
    emit thumbnailFinished(id, result);
}
} // namespace Imv

#include "imvcache.moc"
