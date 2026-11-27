// SPDX-License-Identifier: LGPL-3.0-only
#include "image-provider.h"

#include <QCoreApplication>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QThread>

namespace Imv {
namespace {
ProviderSnapshot snapshot(QImage image, QSize original) {
    if (image.isNull())
        return {{}, {}, {"open", "unreadable", "Image provider did not return CPU pixels"}};
    if (!original.isValid() || original.isEmpty())
        original = image.size();
    if (qint64(original.width()) * original.height() > MaxSourcePixels ||
        qint64(image.width()) * image.height() > MaxSourcePixels) {
        return {{}, {}, {"inspect", "too-large", "Provider frame exceeds 64 MiPixels"}};
    }
    // The provider is application code and returns already-decoded pixels.
    // QImage can alias mutable application memory even in the required format.
    auto owned = image.format() == QImage::Format_RGBA8888
                     ? image.copy()
                     : image.convertToFormat(QImage::Format_RGBA8888);
    if (owned.isNull())
        return {{}, {}, {"decode", "output-failed", "Could not own provider pixels"}};
    return {original, std::move(owned), {}};
}

ProviderSnapshot snapshotTexture(std::unique_ptr<QQuickTextureFactory> factory, QSize original,
                                 QString noFactoryDetail) {
    if (!factory)
        return {{}, {}, {"open", "unreadable", std::move(noFactoryDetail)}};
    // A factory can lend rather than transfer its image storage. Retire the
    // factory only after snapshot has copied the image into owned memory.
    if (original.isEmpty())
        original = factory->textureSize();
    return snapshot(factory->image(), original);
}
} // namespace

Decoded rasterizeProvider(const ProviderSnapshot &snapshot, DecodeSpec spec) {
    if (snapshot.error)
        return {{}, {}, snapshot.error};
    const SourceInfo source{"provider", snapshot.original, 1, 1};
    if (spec.mode == DecodeSpec::Inspect)
        return {source, {}, {}};
    QImage image = snapshot.pixels;
    auto target = spec.bounds;
    if (spec.scale == DecodeSpec::Cover) {
        target =
            target.scaled(image.size().boundedTo(target), Qt::KeepAspectRatio).expandedTo({1, 1});
        const auto crop = target.scaled(image.size(), Qt::KeepAspectRatio).expandedTo({1, 1});
        const QRect rect(
            QPoint((image.width() - crop.width()) / 2, (image.height() - crop.height()) / 2), crop);
        if (rect.size() != image.size())
            image = image.copy(rect);
    } else if (spec.scale == DecodeSpec::Stretch) {
        target = target.boundedTo(image.size());
    } else {
        target = image.size()
                     .scaled(target, Qt::KeepAspectRatio)
                     .boundedTo(image.size())
                     .expandedTo({1, 1});
    }
    if (target != image.size())
        image = image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (image.isNull())
        return {{}, {}, {"decode", "output-failed", "Could not fit provider pixels"}};
    return {source, std::move(image), {}};
}

ProviderCapture snapshotProvider(QQmlEngine *engine, const QUrl &url, QObject *receiver,
                                 std::function<void(ProviderSnapshot)> done) {
    auto *app = QCoreApplication::instance();
    auto *provider = app && engine && QThread::currentThread() == app->thread() &&
                             engine->thread() == app->thread()
                         ? dynamic_cast<QQuickImageProvider *>(engine->imageProvider(url.host()))
                         : nullptr;
    if (!provider || !url.isValid() || url.hasFragment()) {
        return ProviderSnapshot{
            {}, {}, {"admission", "invalid-source", "No matching application image provider"}};
    }
    auto id = url.path(QUrl::FullyEncoded).mid(1);
    if (url.hasQuery())
        id += '?' + url.query(QUrl::FullyEncoded);
    QImage image;
    QSize original;
    const QSize nativeSize;
    switch (provider->imageType()) {
    case QQmlImageProviderBase::Image:
        image = provider->requestImage(id, &original, nativeSize);
        break;
    case QQmlImageProviderBase::Pixmap:
        image = provider->requestPixmap(id, &original, nativeSize).toImage();
        break;
    case QQmlImageProviderBase::Texture: {
        auto factory = std::unique_ptr<QQuickTextureFactory>(
            provider->requestTexture(id, &original, nativeSize));
        // requestTexture writes original; evaluate it only after that call.
        return snapshotTexture(std::move(factory), original,
                               QStringLiteral("Image provider did not return CPU pixels"));
    }
    case QQmlImageProviderBase::ImageResponse: {
        auto *async = dynamic_cast<QQuickAsyncImageProvider *>(provider);
        const QPointer guard(receiver);
        auto *response = async ? async->requestImageResponse(id, nativeSize) : nullptr;
        if (!response)
            break;
        auto interest = std::make_shared<std::function<void(ProviderSnapshot)>>(std::move(done));
        QObject::connect(
            response, &QQuickImageResponse::finished, app,
            [response, receiver = guard, weak = std::weak_ptr(interest)] {
                response->deleteLater();
                const auto state = weak.lock();
                if (!state)
                    return;
                const auto callback = std::exchange(*state, {});
                if (!receiver || !callback)
                    return;
                auto factory = std::unique_ptr<QQuickTextureFactory>(response->textureFactory());
                auto error = factory ? QString() : response->errorString();
                callback(snapshotTexture(std::move(factory), {}, std::move(error)));
            },
            Qt::SingleShotConnection);
        return [response = QPointer(response), interest = std::move(interest)]() mutable {
            const bool pending = interest && bool(*interest);
            interest.reset();
            if (pending && response)
                response->cancel();
        };
    }
    }
    return snapshot(std::move(image), original);
}
} // namespace Imv
