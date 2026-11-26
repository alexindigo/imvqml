// SPDX-License-Identifier: LGPL-3.0-only
#include "image-node.h"

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

struct Entry {
    Asset asset;
    std::unique_ptr<QSGTexture> texture;
    qint64 bytes() const { return rgbaBytes(asset->pixels.size()); }
};

class TextureCache {
public:
    QMetaObject::Connection invalidation;

    std::shared_ptr<Entry> acquire(QQuickWindow *window, const Asset &asset) {
        const auto found = m_index.constFind(asset->key);
        if (found != m_index.cend()) {
            m_lru.splice(m_lru.begin(), m_lru, *found);
            return m_lru.front();
        }
        auto texture = std::unique_ptr<QSGTexture>(window->createTextureFromImage(asset->pixels));
        if (!texture)
            return {};
        uploads.fetch_add(1, std::memory_order_relaxed);
        auto entry = std::make_shared<Entry>(Entry{asset, std::move(texture)});
        if (asset->transient)
            return entry;
        m_lru.push_front(entry);
        m_index.insert(asset->key, m_lru.begin());
        m_bytes += entry->bytes();
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
            m_index.remove((*cursor)->asset->key);
            cursor = m_lru.erase(cursor);
        }
    }

private:
    std::list<std::shared_ptr<Entry>> m_lru;
    QHash<QByteArray, decltype(m_lru)::iterator> m_index;
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

class ImageNode : public QSGClipNode {
public:
    explicit ImageNode(const std::shared_ptr<TextureCache> &cache) : m_cache(cache) {
        appendChildNode(m_image);
        setGeometry(new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 0));
        setFlag(OwnsGeometry);
    }

    ~ImageNode() override {
        delete m_image;
        m_lease.reset();
        if (auto cache = m_cache.lock())
            cache->trim();
    }

    bool sync(QQuickWindow *window, const Asset &asset) {
        if (m_lease && m_lease->asset == asset)
            return true;
        const auto cache = m_cache.lock();
        auto lease = cache ? cache->acquire(window, asset) : nullptr;
        if (!lease)
            return false;
        m_image->setTexture(lease->texture.get());
        m_lease = std::move(lease);
        cache->trim();
        return true;
    }

    void layout(QSizeF size, ImvImage::FillMode fill, ImvImage::Mask mask, qreal radius,
                bool smooth) {
        const auto pixels = QSizeF(m_lease->asset->pixels.size());
        const QRectF box(QPointF{}, size);
        QRectF source(QPointF{}, pixels), target = box;
        if (fill == ImvImage::Contain) {
            target.setSize(pixels.scaled(size, Qt::KeepAspectRatio));
            target.moveCenter(box.center());
        } else if (fill == ImvImage::Cover) {
            source.setSize(size.scaled(pixels, Qt::KeepAspectRatio));
            source.moveCenter(QRectF(QPointF{}, pixels).center());
        }
        m_image->setRect(target);
        m_image->setSourceRect(source);
        m_image->setFiltering(smooth ? QSGTexture::Linear : QSGTexture::Nearest);

        QRectF shape = box;
        if (mask == ImvImage::Circle) {
            radius = qMin(size.width(), size.height()) / 2;
            shape.setSize({radius * 2, radius * 2});
            shape.moveCenter(box.center());
        } else {
            radius =
                mask == ImvImage::Rounded ? qMin(radius, qMin(size.width(), size.height()) / 2) : 0;
        }
        if (shape == m_shape && radius == m_radius)
            return;
        m_shape = shape;
        m_radius = radius;
        setClipRect(shape);
        setIsRectangular(radius <= 0);
        auto *mesh = geometry();
        if (radius <= 0) {
            mesh->allocate(4);
            mesh->setDrawingMode(QSGGeometry::DrawTriangleStrip);
            QSGGeometry::updateRectGeometry(mesh, shape);
        } else {
            // Triangle strip with the perimeter alternating against the center;
            // triangle fans are not available on every RHI backend.
            constexpr int Steps = 16;
            constexpr int Points = (Steps + 1) * 4;
            mesh->allocate(Points * 2 + 1);
            mesh->setDrawingMode(QSGGeometry::DrawTriangleStrip);
            auto *vertices = mesh->vertexDataAsPoint2D();
            const QPointF centers[]{{shape.right() - radius, shape.bottom() - radius},
                                    {shape.left() + radius, shape.bottom() - radius},
                                    {shape.left() + radius, shape.top() + radius},
                                    {shape.right() - radius, shape.top() + radius}};
            for (int i = 0; i < Points; ++i) {
                const int corner = i / (Steps + 1);
                const auto angle = (corner + qreal(i % (Steps + 1)) / Steps) * (M_PI / 2);
                vertices[i * 2].set(centers[corner].x() + qCos(angle) * radius,
                                    centers[corner].y() + qSin(angle) * radius);
                vertices[i * 2 + 1].set(shape.center().x(), shape.center().y());
            }
            vertices[Points * 2] = vertices[0];
        }
        markDirty(DirtyGeometry);
    }

private:
    std::weak_ptr<TextureCache> m_cache;
    std::shared_ptr<Entry> m_lease;
    QSGSimpleTextureNode *m_image = new QSGSimpleTextureNode;
    QRectF m_shape;
    qreal m_radius = -1;
};
} // namespace

QSGNode *updateImageNode(QSGNode *old, QQuickWindow *window, const Asset &asset, QSizeF size,
                         ImvImage::FillMode fill, ImvImage::Mask mask, qreal radius, bool smooth) {
    auto node = std::unique_ptr<ImageNode>(static_cast<ImageNode *>(old));
    if (!asset || size.isEmpty())
        return nullptr;
    if (!node)
        node = std::make_unique<ImageNode>(forGraph(window));
    if (!node->sync(window, asset))
        return nullptr;
    node->layout(size, fill, mask, radius, smooth);
    return node.release();
}

quint64 textureUploads() { return uploads.load(std::memory_order_relaxed); }
} // namespace Imv
