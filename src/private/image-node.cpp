// SPDX-License-Identifier: LGPL-3.0-only
#include "image-node.h"

#include <QDataStream>
#include <QPainter>
#include <QPainterPath>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QtMath>
#include <atomic>
#include <list>
#include <unordered_map>

namespace Imv {
namespace {
constexpr qint64 TextureBudget = 128ll * 1024 * 1024;
std::atomic<quint64> uploads;
std::atomic<QQuickWindow *> failNext;

struct Layout {
    QRectF target, source;
};

Layout layoutImage(QSizeF size, QSizeF pixels, ImvImage::FillMode fill) {
    const QRectF box(QPointF{}, size);
    Layout result{box, QRectF(QPointF{}, pixels)};
    if (fill == ImvImage::Contain) {
        result.target.setSize(pixels.scaled(size, Qt::KeepAspectRatio));
        result.target.moveCenter(box.center());
    } else if (fill == ImvImage::Cover) {
        result.source.setSize(size.scaled(pixels, Qt::KeepAspectRatio));
        result.source.moveCenter(QRectF(QPointF{}, pixels).center());
    }
    return result;
}

struct Presentation {
    QSizeF size;
    QSize raster;
    ImvImage::FillMode fill;
    ImvImage::Mask mask;
    qreal radius;
    QRgb tint;
    bool smooth;

    bool needsRaster() const { return mask != ImvImage::NoMask; }

    QByteArray key(const Asset &asset) const {
        QByteArray bytes;
        QDataStream key(&bytes, QIODeviceBase::WriteOnly);
        key << asset->key << quint8(mask) << quint32(tint);
        if (needsRaster())
            key << size << raster << quint8(fill) << radius << smooth;
        return bytes;
    }

    QImage image(const QImage &pixels) const {
        if (!needsRaster() && !tint)
            return pixels;
        const QSize output = needsRaster() ? raster : pixels.size();
        if (!rgbaBytes(output))
            return {};
        QImage result(output, QImage::Format_ARGB32_Premultiplied);
        if (result.isNull())
            return {};
        result.fill(Qt::transparent);
        QPainter painter(&result);
        const QRectF box(QPointF{}, needsRaster() ? size : QSizeF(output));
        if (needsRaster()) {
            painter.scale(raster.width() / size.width(), raster.height() / size.height());
            painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth);
            const auto placement = layoutImage(size, pixels.size(), fill);
            painter.drawImage(placement.target, pixels, placement.source);
        } else {
            painter.drawImage(QPoint(0, 0), pixels);
        }
        if (tint) {
            // Opaque tint replaces RGB while preserving the fitted image's alpha.
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(box, QColor::fromRgb(tint));
        }
        if (mask == ImvImage::NoMask) {
            painter.end();
            return result;
        }
        QPainterPath outside;
        outside.addRect(box);
        if (mask == ImvImage::Circle) {
            QRectF disk(QPointF{}, QSizeF(radius * 2, radius * 2));
            disk.moveCenter(box.center());
            outside.addEllipse(disk);
        } else {
            outside.addRoundedRect(box, radius, radius);
        }
        // Odd-even fill removes the area outside the mask, while preserving
        // antialiased alpha and the source's existing transparency inside.
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.fillPath(outside, Qt::black);
        painter.end();
        return result;
    }
};

struct Entry {
    Asset asset;
    QByteArray key;
    QSize size;
    std::unique_ptr<QSGTexture> texture;
    qint64 bytes() const { return rgbaBytes(size); }
};

class TextureCache {
public:
    QMetaObject::Connection invalidation;

    std::shared_ptr<Entry> acquire(QQuickWindow *window, const Asset &asset,
                                   const Presentation &presentation, QByteArray key) {
        const auto found = m_index.constFind(key);
        if (found != m_index.cend()) {
            m_lru.splice(m_lru.begin(), m_lru, *found);
            return m_lru.front();
        }
        auto image = presentation.image(asset->pixels);
        if (asset->animation.count && image.constBits() == asset->pixels.constBits())
            image = image.copy(); // A software texture must not keep the batch mapping either.
        if (image.isNull())
            return {};
        auto failing = window;
        if (failNext.compare_exchange_strong(failing, nullptr))
            return {};
        auto texture = std::unique_ptr<QSGTexture>(window->createTextureFromImage(image));
        if (!texture)
            return {};
        uploads.fetch_add(1, std::memory_order_relaxed);
        // An animation frame can pin a whole batch or a large encoded stream.
        // The playback/item owns that backing; an idle texture has no use for it.
        const Asset reusable =
            asset->animation.count || asset->stream || asset->transient ? nullptr : asset;
        auto entry = std::make_shared<Entry>(
            Entry{reusable, std::move(key), image.size(), std::move(texture)});
        if (asset->transient)
            return entry;
        m_lru.push_front(entry);
        m_index.insert(entry->key, m_lru.begin());
        m_bytes += entry->bytes();
        if (entry->asset) {
            auto &uses = m_cpuBackings[entry->asset->pixels.constBits()];
            if (!uses++)
                m_bytes += entry->asset->pixels.sizeInBytes();
        }
        trim();
        return entry;
    }

    void trim() {
        auto cursor = m_lru.end();
        while (m_bytes > TextureBudget && cursor != m_lru.begin()) {
            --cursor;
            if (cursor->use_count() != 1)
                continue;
            m_bytes -= (*cursor)->bytes();
            if (const auto &asset = (*cursor)->asset) {
                const auto bits = asset->pixels.constBits();
                auto backing = m_cpuBackings.find(bits);
                if (!--backing.value()) {
                    m_bytes -= asset->pixels.sizeInBytes();
                    m_cpuBackings.erase(backing);
                }
            }
            m_index.remove((*cursor)->key);
            cursor = m_lru.erase(cursor);
        }
    }

private:
    std::list<std::shared_ptr<Entry>> m_lru;
    QHash<QByteArray, decltype(m_lru)::iterator> m_index;
    QHash<const uchar *, int> m_cpuBackings;
    qint64 m_bytes = 0;
};

std::shared_ptr<TextureCache> forGraph(QQuickWindow *window) {
    // Basic render loops may share a thread, never a window's cache. Every
    // access (including invalidation) runs on the scene graph's render thread.
    thread_local std::unordered_map<QQuickWindow *, std::shared_ptr<TextureCache>> graphs;
    auto &cache = graphs[window];
    if (!cache) {
        cache = std::make_shared<TextureCache>();
        cache->invalidation = QObject::connect(
            window, &QQuickWindow::sceneGraphInvalidated, window,
            [window] {
                if (const auto it = graphs.find(window); it != graphs.end()) {
                    QObject::disconnect(it->second->invalidation);
                    graphs.erase(it);
                }
            },
            Qt::DirectConnection);
    }
    return cache;
}

class ImageNode : public QSGNode {
public:
    explicit ImageNode(const std::shared_ptr<TextureCache> &cache) : m_cache(cache) {
        appendChildNode(m_image);
    }

    ~ImageNode() override {
        delete m_image;
        m_lease.reset();
        if (auto cache = m_cache.lock())
            cache->trim();
    }

    quint64 sourceEpoch = 0;

    bool sync(QQuickWindow *window, const Asset &asset, const Presentation &presentation) {
        auto key = presentation.key(asset);
        if (m_lease && m_lease->key == key)
            return true;
        const auto cache = m_cache.lock();
        auto lease = cache ? cache->acquire(window, asset, presentation, std::move(key)) : nullptr;
        if (!lease)
            return false;
        m_image->setTexture(lease->texture.get());
        m_lease = std::move(lease);
        cache->trim();
        return true;
    }

    void layout(const Presentation &presentation) {
        const auto pixels = QSizeF(m_lease->size);
        const auto placement =
            !presentation.needsRaster()
                ? layoutImage(presentation.size, pixels, presentation.fill)
                : Layout{QRectF(QPointF{}, presentation.size), QRectF(QPointF{}, pixels)};
        m_image->setRect(placement.target);
        m_image->setSourceRect(placement.source);
        m_image->setFiltering(presentation.smooth ? QSGTexture::Linear : QSGTexture::Nearest);
    }

private:
    std::weak_ptr<TextureCache> m_cache;
    std::shared_ptr<Entry> m_lease;
    QSGSimpleTextureNode *m_image = new QSGSimpleTextureNode;
};
} // namespace

TextureOutcome updateImageNode(QSGNode *old, QQuickWindow *window, const Asset &asset, QSizeF size,
                               ImvImage::FillMode fill, ImvImage::Mask mask, qreal radius,
                               const QColor &tint, bool smooth, quint64 sourceEpoch) {
    auto node = std::unique_ptr<ImageNode>(static_cast<ImageNode *>(old));
    if (!asset || size.isEmpty())
        return {nullptr, false};
    if (node && node->sourceEpoch != sourceEpoch)
        node.reset();
    const auto limit = qMin(size.width(), size.height()) / 2;
    radius = mask == ImvImage::Circle ? limit : qMin(radius, limit);
    if (mask == ImvImage::NoMask || radius <= 0) {
        mask = ImvImage::NoMask;
        radius = 0;
    }
    const auto dpr = window->effectiveDevicePixelRatio();
    const auto edge = [dpr](qreal logical) {
        return qCeil(qMin(qreal(MaxEdge + 1), logical * dpr));
    };
    const QRgb tintRgb = tint.isValid() && tint.alphaF() > 0 ? tint.rgb() : 0;
    const Presentation presentation{
        size, QSize(edge(size.width()), edge(size.height())), fill, mask, radius, tintRgb, smooth};
    const bool hadTexture = bool(node);
    if (!node) {
        node = std::make_unique<ImageNode>(forGraph(window));
        node->sourceEpoch = sourceEpoch;
    }
    if (!node->sync(window, asset, presentation))
        return {hadTexture ? node.release() : nullptr, false};
    node->layout(presentation);
    return {node.release(), true};
}

quint64 textureUploads() { return uploads.load(std::memory_order_relaxed); }
void failNextTextureUpload(QQuickWindow *window) { failNext.store(window); }
} // namespace Imv
