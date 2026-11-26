// SPDX-License-Identifier: LGPL-3.0-only
#include "image-provider.h"

#include <QQmlEngine>
#include <QQuickImageProvider>

namespace Imv {
namespace {
void deliver(QImage image, QSize original, DecodeSpec spec, const ImagePipeline::Done &done) {
    if (image.isNull()) {
        done({}, {"open", "unreadable", "Image provider did not return CPU pixels"});
        return;
    }
    if (!original.isValid() || original.isEmpty())
        original = image.size();
    if (qint64(original.width()) * original.height() > MaxSourcePixels ||
        qint64(image.width()) * image.height() > MaxSourcePixels) {
        done({}, {"inspect", "too-large", "Provider frame exceeds 64 MiPixels"});
        return;
    }
    // The provider is application code and returns already-decoded pixels.
    // Copy into an owned RGBA value; no codec runs on the GUI thread.
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
    if (image.isNull()) {
        done({}, {"decode", "output-failed", "Could not normalize provider pixels"});
        return;
    }
    done(imagePipeline().providerAsset(image.copy(), original, spec), {});
}
} // namespace

std::function<void()> requestProvider(QQmlEngine *engine, const QUrl &url, DecodeSpec spec,
                                      QObject *receiver, ImagePipeline::Done done) {
    auto *provider =
        engine ? dynamic_cast<QQuickImageProvider *>(engine->imageProvider(url.host())) : nullptr;
    if (!provider || !url.isValid() || url.hasFragment()) {
        done({}, {"admission", "invalid-source", "No matching application image provider"});
        return {};
    }
    auto id = url.path(QUrl::FullyEncoded).mid(1);
    if (url.hasQuery())
        id += '?' + url.query(QUrl::FullyEncoded);
    QImage image;
    QSize original;
    switch (provider->imageType()) {
    case QQmlImageProviderBase::Image:
        image = provider->requestImage(id, &original, spec.bounds);
        break;
    case QQmlImageProviderBase::Pixmap:
        image = provider->requestPixmap(id, &original, spec.bounds).toImage();
        break;
    case QQmlImageProviderBase::Texture: {
        const auto factory = std::unique_ptr<QQuickTextureFactory>(
            provider->requestTexture(id, &original, spec.bounds));
        if (factory)
            image = factory->image();
        break;
    }
    case QQmlImageProviderBase::ImageResponse: {
        auto *async = dynamic_cast<QQuickAsyncImageProvider *>(provider);
        auto *response = async ? async->requestImageResponse(id, spec.bounds) : nullptr;
        if (!response)
            break;
        auto interest = std::make_shared<ImagePipeline::Done>(std::move(done));
        QObject::connect(
            response, &QQuickImageResponse::finished, &imagePipeline(),
            [response, receiver = QPointer(receiver), spec, weak = std::weak_ptr(interest)] {
                response->deleteLater();
                const auto callback = weak.lock();
                if (!receiver || !callback)
                    return;
                const auto factory =
                    std::unique_ptr<QQuickTextureFactory>(response->textureFactory());
                if (!factory) {
                    (*callback)({}, {"open", "unreadable", response->errorString()});
                    return;
                }
                deliver(factory->image(), factory->textureSize(), spec, *callback);
            },
            Qt::QueuedConnection);
        return [response = QPointer(response), interest = std::move(interest)]() mutable {
            if (interest.use_count() == 1 && response)
                response->cancel();
            interest.reset();
        };
    }
    }
    deliver(std::move(image), original, spec, done);
    return {};
}
} // namespace Imv
