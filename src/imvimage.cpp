// SPDX-License-Identifier: LGPL-3.0-only
#include "imvimage.h"

#include "private/image-node.h"
#include "private/image-pipeline.h"
#include "private/image-provider.h"
#include "private/playback.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <cmath>
#include <optional>

namespace Imv {
namespace {
// Reuse the current physical-size bucket when each edge moves less than
// 12.5% (and less than 4 pixels is always treated as jitter).
bool nearSize(QSize old, QSize next) {
    const auto near = [](int a, int b) { return qAbs(a - b) < qMax(4, a / 8); };
    return !old.isEmpty() && near(old.width(), next.width()) && near(old.height(), next.height());
}

bool displayVisible(const QQuickItem &item) {
    return item.isVisible() && item.window() && item.window()->isVisible() &&
           item.window()->visibility() != QWindow::Minimized;
}

QSize displayBounds(const QQuickItem &item, qreal dpr) {
    if (!item.window() || !std::isfinite(item.width()) || !std::isfinite(item.height()) ||
        item.width() <= 0 || item.height() <= 0)
        return {};
    const auto edge = [dpr](qreal dimension) {
        return int(qMin(qreal(MaxEdge + 1), std::ceil(dimension * dpr)));
    };
    return {edge(item.width()), edge(item.height())};
}
} // namespace

struct ImvImage::State {
    QUrl url;
    QByteArray bytes;
    bool hasBytes = false;
    std::optional<CapturedSource> captured;
    Failure sourceError;
    std::shared_ptr<std::optional<ProviderSnapshot>> provider;
    std::function<void()> cancelProvider;
    FillMode fill = Contain;
    Mask mask = NoMask;
    qreal radius = 0;
    QColor tint = Qt::transparent;
    bool animate = true;
    bool playing = true;
    double videoPosition = 0;
    Status status = Idle;
    Failure failure;
    QVariantMap info;
    QSize requested;
    qreal dpr = 0;
    quint64 generation = 0;
    quint64 assetGeneration = 0;
    quint64 sourceEpoch = 0;
    quint64 presentationRevision = 0;
    quint64 renderAttempt = 0;
    quint64 reportedRevision = 0;
    std::optional<bool> lastUpload;
    Asset asset;
    std::unique_ptr<Playback> playback;
    QMetaObject::Connection windowVisibility;
    ImagePipeline::Interest interest;

    bool matches(QSize bounds, qreal pixelRatio) const {
        return bounds == requested || (pixelRatio == dpr && rgbaBytes(bounds) &&
                                       rgbaBytes(requested) && nearSize(requested, bounds));
    }

    void revoke() {
        ++generation;
        playback.reset();
        interest.reset();
    }

    void cancelSnapshot() {
        if (auto cancel = std::exchange(cancelProvider, {}))
            cancel();
    }
};

ImvImage::ImvImage(QQuickItem *parent) : QQuickItem(parent), d(std::make_unique<State>()) {
    setFlag(ItemHasContents);
    connect(this, &QQuickItem::smoothChanged, this, &ImvImage::refreshPresentation);
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        refreshPresentation();
        QObject::disconnect(d->windowVisibility);
        if (window)
            d->windowVisibility =
                connect(window, &QWindow::visibilityChanged, this, &ImvImage::syncPlayback);
        syncPlayback();
        polish();
    });
    connect(this, &QQuickItem::visibleChanged, this, &ImvImage::syncPlayback);
}

ImvImage::~ImvImage() {
    d->revoke();
    d->provider.reset();
    d->cancelSnapshot();
}

QUrl ImvImage::source() const { return d->url; }
void ImvImage::setSource(const QUrl &source) {
    QUrl resolved = source;
    if (source.isRelative() && !source.path().isEmpty() && !source.path().startsWith('/')) {
        if (const auto context = qmlContext(this))
            resolved = context->resolvedUrl(source);
    }
    if (!d->hasBytes && d->url == resolved)
        return;
    d->hasBytes = false;
    d->bytes.clear();
    d->url = resolved;
    d->captured.reset();
    d->sourceError = {};
    if (!resolved.isEmpty() && resolved.scheme() != "image") {
        auto capture = captureSource(resolved);
        if (auto *error = std::get_if<Failure>(&capture))
            d->sourceError = std::move(*error);
        else
            d->captured = std::get<CapturedSource>(std::move(capture));
    }
    const QPointer guard(this);
    const auto pending = resolved.scheme() == "image"
                             ? std::make_shared<std::optional<ProviderSnapshot>>()
                             : nullptr;
    d->provider = pending;
    d->cancelSnapshot();
    if (!guard || d->url != resolved || d->provider != pending || d->hasBytes)
        return;
    if (pending) {
        auto capture = snapshotProvider(qmlEngine(this), resolved, this,
                                        [guard, pending](ProviderSnapshot value) {
                                            if (!guard || guard->d->provider != pending)
                                                return;
                                            pending->emplace(std::move(value));
                                            guard->polish();
                                        });
        if (!guard || d->provider != pending) {
            if (auto *cancel = std::get_if<std::function<void()>>(&capture))
                (*cancel)();
            return;
        }
        if (auto *snapshot = std::get_if<ProviderSnapshot>(&capture)) {
            if (snapshot->error)
                d->sourceError = std::move(snapshot->error);
            else
                pending->emplace(std::move(*snapshot));
        } else {
            d->cancelProvider = std::get<std::function<void()>>(std::move(capture));
        }
    }
    resetRequest(true);
    if (guard && d->url == resolved && d->provider == pending && !d->hasBytes)
        emit sourceChanged();
}

void ImvImage::setBytes(const QByteArray &bytes) {
    const bool urlChanged = !d->url.isEmpty();
    d->hasBytes = true;
    d->bytes = bytes;
    d->bytes.detach();
    d->url.clear();
    d->captured.reset();
    d->sourceError = {};
    const QPointer guard(this);
    d->provider.reset();
    d->cancelSnapshot();
    if (!guard || !d->hasBytes)
        return;
    resetRequest(true);
    if (guard && urlChanged)
        emit sourceChanged();
}

ImvImage::FillMode ImvImage::fillMode() const { return d->fill; }
void ImvImage::setFillMode(FillMode mode) {
    if (mode < Contain || mode > Stretch || d->fill == mode)
        return;
    d->fill = mode;
    const QPointer guard(this);
    resetRequest(true);
    if (guard)
        emit fillModeChanged();
}

ImvImage::Mask ImvImage::mask() const { return d->mask; }
void ImvImage::setMask(Mask mask) {
    if (mask < NoMask || mask > Rounded || d->mask == mask)
        return;
    d->mask = mask;
    refreshPresentation();
    emit maskChanged();
}

qreal ImvImage::radius() const { return d->radius; }
void ImvImage::setRadius(qreal radius) {
    radius = std::isfinite(radius) ? qMax(qreal(0), radius) : 0;
    if (d->radius == radius)
        return;
    d->radius = radius;
    refreshPresentation();
    emit radiusChanged();
}

QColor ImvImage::tint() const { return d->tint; }
void ImvImage::setTint(const QColor &color) {
    if (d->tint == color)
        return;
    d->tint = color;
    refreshPresentation();
    emit tintChanged();
}

bool ImvImage::animate() const { return d->animate; }
void ImvImage::setAnimate(bool animate) {
    if (std::exchange(d->animate, animate) != animate) {
        if (d->playback && !animate)
            d->playback->rewind();
        const QPointer guard(this);
        if (!d->provider && (d->hasBytes || !d->url.isEmpty()))
            resetRequest(false);
        if (guard)
            emit animateChanged();
    }
}

bool ImvImage::playing() const { return d->playing; }
void ImvImage::setPlaying(bool playing) {
    if (std::exchange(d->playing, playing) != playing) {
        syncPlayback();
        emit playingChanged();
    }
}

void ImvImage::syncPlayback() {
    const bool visible = displayVisible(*this);
    if (d->interest)
        imagePipeline().setVisible(d->interest, visible);
    if (d->playback)
        d->playback->run(d->status == Ready && d->animate && d->playing && visible);
}

double ImvImage::videoPosition() const { return d->videoPosition; }
void ImvImage::setVideoPosition(double position) {
    if (!std::isfinite(position) || position < 0 || position > 1 || d->videoPosition == position)
        return;
    d->videoPosition = position;
    const QPointer guard(this);
    resetRequest(true);
    if (guard)
        emit videoPositionChanged();
}

ImvImage::Status ImvImage::status() const { return d->status; }
QString ImvImage::errorStage() const { return d->failure.stage; }
QString ImvImage::errorCode() const { return d->failure.code; }
QString ImvImage::errorDetail() const { return d->failure.detail; }
QVariantMap ImvImage::sourceInfo() const { return d->info; }

void ImvImage::setOutcome(Status status, QString stage, QString code, QString detail) {
    const bool statusChange = std::exchange(d->status, status) != status;
    const bool errorChange =
        d->failure.stage != stage || d->failure.code != code || d->failure.detail != detail;
    d->failure = {std::move(stage), std::move(code), std::move(detail)};
    syncPlayback();
    const auto generation = d->generation;
    QPointer guard(this);
    if (errorChange)
        emit errorChanged();
    if (guard && generation == d->generation && statusChange)
        emit statusChanged();
}

void ImvImage::resetRequest(bool discard) {
    d->revoke();
    d->requested = {};
    if (discard) {
        ++d->sourceEpoch;
        d->asset.reset();
    }
    const bool hadInfo = discard && !d->info.isEmpty();
    if (hadInfo)
        d->info.clear();
    polish();
    refreshPresentation();
    const QPointer guard(this);
    const auto generation = d->generation;
    if (d->sourceError)
        setOutcome(Failed, d->sourceError.stage, d->sourceError.code, d->sourceError.detail);
    else
        setOutcome(!d->hasBytes && d->url.isEmpty() ? Idle : Loading);
    if (guard && generation == d->generation && hadInfo)
        emit sourceInfoChanged();
}

void ImvImage::refreshPresentation() {
    ++d->presentationRevision;
    update();
}

void ImvImage::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemDevicePixelRatioHasChanged)
        polish();
}

void ImvImage::geometryChange(const QRectF &now, const QRectF &before) {
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size()) {
        const auto dpr = window() ? window()->effectiveDevicePixelRatio() : 1;
        if (d->interest && d->status == Loading && !d->matches(displayBounds(*this, dpr), dpr)) {
            d->interest.reset();
            d->requested = {};
        }
        polish();
        refreshPresentation();
    }
}

void ImvImage::updatePolish() {
    if (d->sourceError || (!d->hasBytes && d->url.isEmpty()))
        return;
    if (d->provider && !*d->provider)
        return;
    const auto dpr = window() ? window()->effectiveDevicePixelRatio() : 1;
    const auto bounds = displayBounds(*this, dpr);
    if (bounds.isEmpty()) {
        if (!d->requested.isEmpty()) {
            resetRequest(false);
        }
        return;
    }
    if (d->matches(bounds, dpr))
        return;
    d->revoke();
    d->dpr = dpr;
    d->requested = bounds;
    auto scale = DecodeSpec::Contain;
    if (d->fill == Cover)
        scale = DecodeSpec::Cover;
    else if (d->fill == Stretch)
        scale = DecodeSpec::Stretch;
    const DecodeSpec spec{!d->provider && d->animate ? DecodeSpec::Animation : DecodeSpec::Pixels,
                          bounds, scale, 0, d->videoPosition};
    const auto generation = d->generation;
    const auto guard = QPointer(this);
    setOutcome(Loading);
    if (!guard || generation != d->generation)
        return;
    const auto done = [self = guard, generation](Asset asset, Failure error) {
        if (!self || self->d->generation != generation)
            return;
        auto &state = *self->d;
        state.asset = std::move(asset);
        state.assetGeneration = generation;
        if (state.asset && (state.asset->stream || state.asset->animation.count)) {
            state.playback = std::make_unique<Playback>(
                state.asset, [self, generation](Asset frame, Failure failure) {
                    if (!self || self->d->generation != generation)
                        return;
                    if (failure) {
                        self->setOutcome(Failed, failure.stage, failure.code, failure.detail);
                        return;
                    }
                    self->d->asset = std::move(frame);
                    self->refreshPresentation();
                });
        }
        self->refreshPresentation();
        const auto info = state.asset ? asMap(state.asset->info) : QVariantMap{};
        const bool infoChange = info != state.info;
        state.info = info;
        if (error)
            self->setOutcome(Failed, error.stage, error.code, error.detail);
        if (self && self->d->generation == generation && infoChange)
            emit self->sourceInfoChanged();
    };
    if (!spec.valid()) {
        done({}, {"admission", "invalid-size", "Physical size exceeds the output edge limit"});
    } else if (d->provider) {
        auto decoded = rasterizeProvider(**d->provider, spec);
        if (decoded.error)
            done({}, std::move(decoded.error));
        else
            done(
                imagePipeline().providerAsset(std::move(decoded.pixels), decoded.source.size, spec),
                {});
    } else {
        auto source = d->hasBytes ? SourceInput(d->bytes) : SourceInput(*d->captured);
        d->interest = imagePipeline().request(std::move(source), spec, done, displayVisible(*this));
    }
}

QSGNode *ImvImage::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    const auto asset = d->asset;
    const bool recreated = !old;
    const auto result = updateImageNode(old, window(), asset, size(), d->fill, d->mask, d->radius,
                                        d->tint, smooth(), d->sourceEpoch);
    const auto revision = d->presentationRevision;
    if (asset && !size().isEmpty() && d->assetGeneration == d->generation &&
        (recreated || d->reportedRevision != revision || d->lastUpload != result.uploaded)) {
        d->reportedRevision = revision;
        d->lastUpload = result.uploaded;
        const auto attempt = ++d->renderAttempt;
        QMetaObject::invokeMethod(
            this,
            [this, generation = d->generation, revision, attempt, success = result.uploaded] {
                if (d->generation != generation || d->presentationRevision != revision ||
                    d->renderAttempt != attempt || (d->failure && d->failure.stage != "upload"))
                    return;
                if (success)
                    setOutcome(Ready);
                else
                    setOutcome(Failed, "upload", "texture-failed",
                               "Could not create a scene graph texture");
            },
            Qt::QueuedConnection);
    }
    return result.node;
}
} // namespace Imv
