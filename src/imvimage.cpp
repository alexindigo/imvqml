// SPDX-License-Identifier: LGPL-3.0-only
#include "imvimage.h"

#include "private/image-node.h"
#include "private/image-provider.h"
#include "private/playback.h"
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWindow>
#include <cmath>

namespace Imv {
namespace {
// Reuse the current physical-size bucket when each edge moves less than
// 12.5% (and less than 4 pixels is always treated as jitter).
bool nearSize(QSize old, QSize next) {
    const auto near = [](int a, int b) { return qAbs(a - b) < qMax(4, a / 8); };
    return !old.isEmpty() && near(old.width(), next.width()) && near(old.height(), next.height());
}
} // namespace

struct ImvImage::State {
    QUrl url;
    QByteArray bytes;
    bool hasBytes = false;
    FillMode fill = Contain;
    Mask mask = NoMask;
    qreal radius = 0;
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
    quint64 reported = 0; // Last generation queued by scene graph sync.
    Asset asset;
    std::unique_ptr<Playback> playback;
    QMetaObject::Connection windowVisibility;
    ImagePipeline::Interest interest;
    std::function<void()> cancelProvider;

    void revoke() {
        ++generation;
        playback.reset();
        interest.reset();
        if (auto cancel = std::exchange(cancelProvider, {}))
            cancel();
    }
};

ImvImage::ImvImage(QQuickItem *parent) : QQuickItem(parent), d(std::make_unique<State>()) {
    setFlag(ItemHasContents);
    connect(this, &QQuickItem::smoothChanged, this, [this] { update(); });
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *window) {
        QObject::disconnect(d->windowVisibility);
        if (window)
            d->windowVisibility =
                connect(window, &QWindow::visibilityChanged, this, &ImvImage::syncPlayback);
        syncPlayback();
        polish();
    });
    connect(this, &QQuickItem::visibleChanged, this, &ImvImage::syncPlayback);
}

ImvImage::~ImvImage() { d->revoke(); }

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
    const QPointer guard(this);
    resetRequest(true);
    if (guard)
        emit sourceChanged();
}

void ImvImage::setBytes(const QByteArray &bytes) {
    const bool urlChanged = !d->url.isEmpty();
    d->hasBytes = true;
    d->bytes = bytes;
    d->bytes.detach();
    d->url.clear();
    const QPointer guard(this);
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
    update();
    emit maskChanged();
}

qreal ImvImage::radius() const { return d->radius; }
void ImvImage::setRadius(qreal radius) {
    radius = std::isfinite(radius) ? qMax(qreal(0), radius) : 0;
    if (d->radius == radius)
        return;
    d->radius = radius;
    update();
    emit radiusChanged();
}

bool ImvImage::animate() const { return d->animate; }
void ImvImage::setAnimate(bool animate) {
    if (std::exchange(d->animate, animate) != animate) {
        if (d->playback && !animate)
            d->playback->rewind();
        syncPlayback();
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
    if (d->playback)
        d->playback->run(d->status == Ready && d->animate && d->playing && isVisible() &&
                         window() && window()->isVisible() &&
                         window()->visibility() != QWindow::Minimized);
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
    if (discard)
        d->asset.reset();
    const bool hadInfo = discard && !d->info.isEmpty();
    if (hadInfo)
        d->info.clear();
    polish();
    update();
    const QPointer guard(this);
    const auto generation = d->generation;
    setOutcome(!d->hasBytes && d->url.isEmpty() ? Idle : Loading);
    if (guard && generation == d->generation && hadInfo)
        emit sourceInfoChanged();
}

void ImvImage::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemDevicePixelRatioHasChanged)
        polish();
}

void ImvImage::geometryChange(const QRectF &now, const QRectF &before) {
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size()) {
        polish();
        update();
    }
}

void ImvImage::updatePolish() {
    if (!d->hasBytes && d->url.isEmpty())
        return;
    if (!window() || !std::isfinite(width()) || !std::isfinite(height()) || width() <= 0 ||
        height() <= 0) {
        if (!d->requested.isEmpty()) {
            resetRequest(false);
        }
        return;
    }
    const auto dpr = window()->effectiveDevicePixelRatio();
    const auto edge = [dpr](qreal dimension) {
        // Keep an out-of-bounds sentinel rather than overflowing on a large item.
        return int(qMin(qreal(MaxEdge + 1), std::ceil(dimension * dpr)));
    };
    const QSize bounds(edge(width()), edge(height()));
    if (bounds == d->requested || (dpr == d->dpr && rgbaBytes(bounds) && rgbaBytes(d->requested) &&
                                   nearSize(d->requested, bounds)))
        return;
    d->revoke();
    d->dpr = dpr;
    d->requested = bounds;
    auto scale = DecodeSpec::Contain;
    if (d->fill == Cover)
        scale = DecodeSpec::Cover;
    else if (d->fill == Stretch)
        scale = DecodeSpec::Stretch;
    const DecodeSpec spec{d->url.scheme() == "image" ? DecodeSpec::Pixels : DecodeSpec::Animation,
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
                    self->update();
                });
        }
        self->update();
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
    } else if (d->url.scheme() == "image") {
        auto cancel = requestProvider(qmlEngine(this), d->url, spec, this, done);
        if (guard && d->generation == generation)
            d->cancelProvider = std::move(cancel);
        else if (cancel)
            cancel();
    } else {
        auto source = d->hasBytes ? openSource(d->bytes) : openSource(d->url);
        d->interest = imagePipeline().request(std::move(source), spec, done);
    }
}

QSGNode *ImvImage::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    const auto asset = d->asset;
    QSGNode *node =
        updateImageNode(old, window(), asset, size(), d->fill, d->mask, d->radius, smooth());
    if (asset && !size().isEmpty() && d->reported != d->generation &&
        d->assetGeneration == d->generation) {
        d->reported = d->generation;
        QMetaObject::invokeMethod(
            this,
            [this, generation = d->generation, success = node != nullptr] {
                if (d->generation != generation)
                    return;
                if (success)
                    setOutcome(Ready);
                else
                    setOutcome(Failed, "upload", "texture-failed",
                               "Could not create a scene graph texture");
            },
            Qt::QueuedConnection);
    }
    return node;
}
} // namespace Imv
