// SPDX-License-Identifier: LGPL-3.0-only
#include "image-node.h"
#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>
#include <array>
#include <imvqml/imvimagejob.h>

class ItemTest : public QObject {
    Q_OBJECT
private slots:
    void display();
    void transientProvider_data();
    void transientProvider();
    void transientProviderJob();
    void transientAsyncProviderJob();
    void masks();
    void alphaTint();
    void tintGeometry();
    void firstFramePreview();
    void animation();
    void cachePolicy();
};

class TestProvider : public QQuickImageProvider {
public:
    TestProvider() : QQuickImageProvider(Image) {}
    QImage requestImage(const QString &id, QSize *original, const QSize &requested) override {
        if (id != "yellow")
            return {};
        const QSize size = requested.isEmpty() ? QSize(64, 64) : requested;
        *original = size;
        QImage image(size, QImage::Format_RGBA8888);
        image.fill(Qt::yellow);
        return image;
    }
};

class BorrowingFactory : public QQuickTextureFactory {
public:
    explicit BorrowingFactory(uchar *storage) : m_storage(storage) {}
    // Return the slot to the provider when the factory's lease ends. Keep the
    // storage allocated so reading it after retirement fails deterministically.
    ~BorrowingFactory() override { std::fill_n(m_storage, textureByteCount(), uchar(0)); }
    QSGTexture *createTexture(QQuickWindow *) const override { return nullptr; }
    QSize textureSize() const override { return {64, 64}; }
    int textureByteCount() const override { return 64 * 64 * 4; }
    QImage image() const override {
        return QImage(m_storage, 64, 64, 64 * 4, QImage::Format_RGBA8888);
    }

private:
    uchar *const m_storage;
};

class TransientProvider : public QQuickImageProvider {
public:
    explicit TransientProvider(ImageType type) : QQuickImageProvider(type) { storage.fill(255); }
    void forget() {
        storage.fill(0);
        available = false;
    }
    QImage requestImage(const QString &, QSize *original, const QSize &) override {
        ++requests;
        onGuiThread = QThread::currentThread() == QCoreApplication::instance()->thread();
        *original = {64, 64};
        return available ? QImage(storage.data(), 64, 64, 64 * 4, QImage::Format_RGBA8888)
                         : QImage();
    }
    QPixmap requestPixmap(const QString &id, QSize *original, const QSize &requested) override {
        return QPixmap::fromImage(requestImage(id, original, requested));
    }
    QQuickTextureFactory *requestTexture(const QString &, QSize *original, const QSize &) override {
        ++requests;
        onGuiThread = QThread::currentThread() == QCoreApplication::instance()->thread();
        *original = {64, 64};
        if (!available)
            return nullptr;
        storage.fill(255);
        return new BorrowingFactory(storage.data());
    }
    int requests = 0;
    bool onGuiThread = false;

private:
    std::array<uchar, 64 * 64 * 4> storage;
    bool available = true;
};

class TransientResponse : public QQuickImageResponse {
public:
    explicit TransientResponse(QImage image) : m_image(std::move(image)) {}
    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    void cancel() override {
        QMetaObject::invokeMethod(this, &TransientResponse::finished, Qt::QueuedConnection);
    }

private:
    QImage m_image;
};

class TransientAsyncProvider : public QQuickAsyncImageProvider {
public:
    QQuickImageResponse *requestImageResponse(const QString &, const QSize &) override {
        storage.fill(255);
        response =
            new TransientResponse(QImage(storage.data(), 16, 16, 16 * 4, QImage::Format_RGBA8888));
        return response;
    }
    void deliver() {
        response->finished();
        storage.fill(0);
    }
    QPointer<TransientResponse> response;

private:
    std::array<uchar, 16 * 16 * 4> storage;
};

void ItemTest::transientProvider_data() {
    QTest::addColumn<int>("type");
    QTest::newRow("Image") << int(QQuickImageProvider::Image);
    QTest::newRow("Pixmap") << int(QQuickImageProvider::Pixmap);
    QTest::newRow("Texture") << int(QQuickImageProvider::Texture);
}

void ItemTest::transientProvider() {
    QFETCH(int, type);
    QQmlEngine engine;
    engine.addImportPath(IMV_IMPORT_PATH);
    auto *provider = new TransientProvider(QQuickImageProvider::ImageType(type));
    engine.addImageProvider("transient", provider);
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQuick
import ImvQml
Window {
    width: 128; height: 128; color: "blue"
    ImvImage { objectName: "preview" }
}
)",
                      QUrl());
    QTRY_VERIFY(!component.isLoading());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const std::unique_ptr<QObject> root(component.create());
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    auto *item = window ? window->findChild<Imv::ImvImage *>("preview") : nullptr;
    QVERIFY(item);
    item->setSource(QUrl("image://transient/once"));
    provider->forget();
    item->setSize({32, 32});
    window->show();
    QTRY_VERIFY(window->isExposed());
    const auto pixel = [window](int x, int y) {
        const auto frame = window->grabWindow();
        const auto scale = frame.devicePixelRatio();
        return frame.isNull() ? QColor() : frame.pixelColor(qRound(x * scale), qRound(y * scale));
    };
    QTRY_COMPARE_WITH_TIMEOUT(pixel(16, 16), QColor(Qt::white), 1500);
    item->setSize({64, 64});
    item->setFillMode(Imv::ImvImage::Cover);
    QTRY_COMPARE_WITH_TIMEOUT(pixel(48, 48), QColor(Qt::white), 1500);
    QTRY_COMPARE(item->status(), Imv::ImvImage::Ready);
    QCOMPARE(provider->requests, 1);
    QVERIFY(provider->onGuiThread);
}

void ItemTest::transientProviderJob() {
    QQmlEngine engine;
    engine.addImportPath(IMV_IMPORT_PATH);
    auto *provider = new TransientProvider(QQuickImageProvider::Texture);
    engine.addImageProvider("transient", provider);
    QQmlComponent component(&engine);
    component.setData("import ImvQml\nImvImageJob {}", QUrl());
    QTRY_VERIFY(!component.isLoading());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const std::unique_ptr<QObject> root(component.create());
    auto *job = qobject_cast<Imv::ImvImageJob *>(root.get());
    QVERIFY(job);
    QSignalSpy done(job, &Imv::ImvImageJob::finished);
    const auto url = QUrl("image://transient/once");
    const int inspect = job->inspect(url);
    const int sample = job->sample(url, 8, 8);
    provider->forget();
    QVERIFY(inspect > 0 && sample > 0);
    QVERIFY(done.isEmpty());
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(done[0][0].toInt(), inspect);
    QCOMPARE(done[1][0].toInt(), sample);
    const auto info = done[0][1].toMap();
    QCOMPARE(info.value("stage").toString(), QString("complete"));
    QCOMPARE(info.value("sourceInfo").toMap().value("width").toInt(), 64);
    QCOMPARE(done[1][1].toMap().value("rgba").toByteArray(), QByteArray(8 * 8 * 4, char(255)));
    QCOMPARE(provider->requests, 2);
    QVERIFY(provider->onGuiThread);
}

void ItemTest::transientAsyncProviderJob() {
    QQmlEngine engine;
    engine.addImportPath(IMV_IMPORT_PATH);
    auto *provider = new TransientAsyncProvider;
    engine.addImageProvider("transient", provider);
    QQmlComponent component(&engine);
    component.setData("import ImvQml\nImvImageJob {}", QUrl());
    QTRY_VERIFY(!component.isLoading());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    const std::unique_ptr<QObject> root(component.create());
    auto *job = qobject_cast<Imv::ImvImageJob *>(root.get());
    QVERIFY(job);
    QSignalSpy done(job, &Imv::ImvImageJob::finished);
    const auto url = QUrl("image://transient/once");
    const int id = job->sample(url, 8, 8);
    QVERIFY(id > 0 && provider->response);
    provider->deliver();
    QVERIFY(done.isEmpty());
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toInt(), id);
    QCOMPARE(done[0][1].toMap().value("rgba").toByteArray(), QByteArray(8 * 8 * 4, char(255)));
    QTRY_VERIFY(!provider->response);
    const int cancelled = job->sample(url, 8, 8);
    QVERIFY(cancelled > 0 && provider->response);
    QVERIFY(job->cancel(cancelled));
    QTRY_COMPARE(done.size(), 2);
    QCOMPARE(done[1][0].toInt(), cancelled);
    QCOMPARE(done[1][1].toMap().value("code").toString(), QString("cancelled"));
    QTRY_VERIFY(!provider->response);
}

void ItemTest::display() {
    QQmlEngine engine;
    engine.addImportPath(IMV_IMPORT_PATH);
    engine.addImageProvider("test", new TestProvider);
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQuick
import ImvQml
Window {
    width: 320; height: 200; color: "#0000ff"
    ImvImage { objectName: "first"; width: 32; height: 32; smooth: false }
    ImvImage { objectName: "second"; x: 64; width: 32; height: 32; smooth: false }
}
)",
                      QUrl());
    QTRY_VERIFY(!component.isLoading());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> root(component.create());
    auto *window = qobject_cast<QQuickWindow *>(root.get());
    QVERIFY(window);
    const auto item = [&](const char *name) { return window->findChild<QQuickItem *>(name); };
    const auto first = qobject_cast<Imv::ImvImage *>(item("first"));
    const auto second = qobject_cast<Imv::ImvImage *>(item("second"));
    QVERIFY(first && second);
    const auto decoded = Imv::imagePipeline().submissions();
    const auto uploaded = Imv::textureUploads();
    QSignalSpy outcomes(first, &Imv::ImvImage::errorChanged);
    const auto source = QUrl::fromLocalFile(IMV_FIXTURES "/palette.png");
    first->setSource(QUrl::fromLocalFile(IMV_FIXTURES "/garbage"));
    first->setSource(source);
    second->setSource(source);
    window->show();
    QTRY_VERIFY(window->isExposed());
    QSignalSpy presentations(window, &QQuickWindow::frameSwapped);
    const auto pixel = [window](int x, int y) {
        const auto frame = window->grabWindow();
        if (frame.isNull())
            return QColor();
        const auto scale = frame.devicePixelRatio();
        return frame.pixelColor(qRound(x * scale), qRound(y * scale));
    };
    QTRY_COMPARE(pixel(8, 16), QColor(Qt::red));
    QCOMPARE(pixel(24, 16), QColor(Qt::green));
    QCOMPARE(pixel(72, 16), QColor(Qt::red));
    QCOMPARE(pixel(8, 4), QColor(Qt::blue));
    QTRY_COMPARE(first->status(), Imv::ImvImage::Ready);
    QTRY_COMPARE(second->status(), Imv::ImvImage::Ready);
    QTRY_VERIFY(!presentations.isEmpty());
    QCOMPARE(Imv::imagePipeline().submissions(), decoded + 1);
    QCOMPARE(Imv::textureUploads(), uploaded + 1);
    QVERIFY(outcomes.isEmpty());

    first->setWidth(33);
    QCOMPARE(pixel(8, 16), QColor(Qt::red));
    QCOMPARE(Imv::imagePipeline().submissions(), decoded + 1);

    first->setWidth(64);
    first->setHeight(64);
    QTRY_COMPARE(pixel(48, 32), QColor(Qt::green));
    QTRY_COMPARE(first->status(), Imv::ImvImage::Ready);
    QCOMPARE(Imv::imagePipeline().submissions(), decoded + 2);
    QCOMPARE(Imv::textureUploads(), uploaded + 2);
    first->setMask(Imv::ImvImage::Circle);
    // (1.5, 17.5) is in the contained image but outside a radius-32 circle.
    QTRY_COMPARE(pixel(1, 17), QColor(Qt::blue));
    QCOMPARE(pixel(48, 32), QColor(Qt::green));
    first->setMask(Imv::ImvImage::Rounded);
    first->setRadius(16);
    first->setFillMode(Imv::ImvImage::Cover);
    QTRY_COMPARE(pixel(48, 4), QColor(Qt::green));
    QCOMPARE(pixel(1, 1), QColor(Qt::blue));
    first->setMask(Imv::ImvImage::NoMask);
    first->setFillMode(Imv::ImvImage::Stretch);
    QTRY_COMPARE(pixel(1, 1), QColor(Qt::red));

    QFile file(IMV_FIXTURES "/palette.svg");
    QVERIFY(file.open(QIODevice::ReadOnly));
    second->setBytes(file.readAll());
    QTRY_COMPARE(second->sourceInfo().value("format").toString(), "svg");
    QCOMPARE(pixel(72, 16), QColor(Qt::red));
    second->setSource(QUrl("image://test/yellow"));
    QTRY_COMPARE(second->status(), Imv::ImvImage::Ready);
    QCOMPARE(pixel(72, 16), QColor(Qt::yellow));
    second->setSource(QUrl("https://invalid.example/never-fetched"));
    QTRY_COMPARE(second->errorCode(), "invalid-source");
    QTRY_COMPARE(pixel(72, 16), QColor(Qt::blue));

    const auto beforeHit = Imv::imagePipeline().submissions();
    const auto beforeReuse = Imv::textureUploads();
    second->setSource(source);
    QTRY_COMPARE(second->status(), Imv::ImvImage::Ready);
    QCOMPARE(pixel(72, 16), QColor(Qt::red));
    QCOMPARE(Imv::imagePipeline().submissions(), beforeHit);
    QCOMPARE(Imv::textureUploads(), beforeReuse);
}

void ItemTest::masks() {
    QQuickWindow window;
    window.resize(128, 96);
    window.setColor(Qt::blue);
    Imv::ImvImage item(window.contentItem());
    item.setSize({96, 64});
    item.setFillMode(Imv::ImvImage::Stretch);
    item.setSmooth(false);
    QImage source(96, 64, QImage::Format_RGBA8888);
    source.fill(Qt::red);
    QBuffer encoded;
    QVERIFY(encoded.open(QIODevice::WriteOnly));
    QVERIFY(source.save(&encoded, "PNG"));
    item.setBytes(encoded.data());
    window.show();
    QTRY_COMPARE(item.status(), Imv::ImvImage::Ready);
    const auto sample = [&] {
        const QImage frame = window.grabWindow();
        if (frame.isNull())
            return QString("no frame");
        const qreal dpr = frame.devicePixelRatio();
        const auto at = [&](int x, int y) {
            return frame.pixelColor(qRound(x * dpr), qRound(y * dpr));
        };
        bool blended = false;
        for (int y = 0; y < 31; ++y) {
            for (int x = 0; x < 48; ++x) {
                const auto p = at(x, y);
                blended = blended || (p.red() > 8 && p.blue() > 8 && p.green() < 8);
            }
        }
        return QString("%1 %2 %3 %4 %5 edge=%6")
            .arg(at(1, 1).name(), at(94, 1).name(), at(1, 62).name(), at(94, 62).name(),
                 at(48, 32).name(), QString::number(blended));
    };
    const QString square("#ff0000 #ff0000 #ff0000 #ff0000 #ff0000 edge=0");
    const QString clipped("#0000ff #0000ff #0000ff #0000ff #ff0000 edge=1");
    QTRY_COMPARE(sample(), square);
    item.setMask(Imv::ImvImage::Circle);
    QTest::qWait(120);
    const auto circle = sample();
    item.setMask(Imv::ImvImage::Rounded);
    item.setRadius(20);
    QTest::qWait(120);
    const auto rounded = sample();
    qInfo() << "graphics API" << window.rendererInterface()->graphicsApi() << "circle" << circle
            << "rounded" << rounded;
    QCOMPARE(circle + '|' + rounded, clipped + '|' + clipped);
    item.setRadius(0);
    QTRY_COMPARE(sample(), square);
}

void ItemTest::alphaTint() {
    QQuickWindow window;
    window.resize(96, 64);
    window.setColor(Qt::black);
    Imv::ImvImage item(window.contentItem());
    item.setSize({96, 64});
    item.setFillMode(Imv::ImvImage::Stretch);
    item.setSmooth(false);
    QImage source(96, 64, QImage::Format_RGBA8888);
    source.fill(Qt::red);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 32; x < source.width(); ++x)
            source.setPixelColor(x, y, QColor(255, 0, 0, x < 64 ? 128 : 0));
    }
    QBuffer encoded;
    QVERIFY(encoded.open(QIODevice::WriteOnly));
    QVERIFY(source.save(&encoded, "PNG"));
    item.setBytes(encoded.data());
    window.show();
    QTRY_COMPARE(item.status(), Imv::ImvImage::Ready);
    const auto sample = [&] {
        const auto frame = window.grabWindow();
        if (frame.isNull())
            return QString("no frame");
        const auto dpr = frame.devicePixelRatio();
        const auto at = [&](int x) { return frame.pixelColor(qRound(x * dpr), qRound(32 * dpr)); };
        return QString("%1 %2 %3").arg(at(16).name(), at(48).name(), at(80).name());
    };
    const QString untinted("#ff0000 #800000 #000000");
    QTRY_COMPARE(sample(), untinted);
    const auto decodes = Imv::imagePipeline().submissions();
    const auto uploads = Imv::textureUploads();
    item.setProperty("tint", QColor(0, 255, 0, 128));
    QTRY_COMPARE_WITH_TIMEOUT(sample(), QString("#00ff00 #008000 #000000"), 1500);
    QVERIFY(item.setProperty("tint", QColor(Qt::blue)));
    QTRY_COMPARE_WITH_TIMEOUT(sample(), QString("#0000ff #000080 #000000"), 1500);
    QVERIFY(item.setProperty("tint", QColor(Qt::green)));
    QTRY_COMPARE_WITH_TIMEOUT(sample(), QString("#00ff00 #008000 #000000"), 1500);
    QCOMPARE(Imv::textureUploads(), uploads + 2);
    QVERIFY(item.setProperty("tint", QColor(Qt::transparent)));
    QTRY_COMPARE_WITH_TIMEOUT(sample(), untinted, 1500);
    QCOMPARE(Imv::textureUploads(), uploads + 2);
    QCOMPARE(Imv::imagePipeline().submissions(), decodes);
    QCOMPARE(item.status(), Imv::ImvImage::Ready);
    qInfo() << "graphics API" << window.rendererInterface()->graphicsApi();
}

void ItemTest::tintGeometry() {
    QQuickWindow window;
    window.resize(96, 96);
    window.setColor(Qt::black);
    Imv::ImvImage item(window.contentItem());
    item.setSize({96, 90});
    item.setSmooth(false);
    QImage source(96, 64, QImage::Format_RGBA8888);
    source.fill(Qt::red);
    for (int y = 0; y < 64; ++y) {
        for (int x = 32; x < 96; ++x)
            source.setPixelColor(x, y, QColor(255, 0, 0, x < 64 ? 128 : 0));
    }
    QBuffer encoded;
    QVERIFY(encoded.open(QIODevice::WriteOnly));
    QVERIFY(source.save(&encoded, "PNG"));
    item.setBytes(encoded.data());
    item.setTint(Qt::green);
    window.show();
    QTRY_COMPARE(item.status(), Imv::ImvImage::Ready);
    const auto sample = [&] {
        const auto frame = window.grabWindow();
        if (frame.isNull())
            return QString("no frame");
        // grabWindow metadata differs across render loops; measure its raster.
        const auto xScale = qreal(frame.width()) / window.width();
        const auto yScale = qreal(frame.height()) / window.height();
        const auto at = [&](int x, int y) {
            return frame.pixelColor(qRound(x * xScale), qRound(y * yScale)).name();
        };
        return QString("%1 %2 %3 %4 %5 %6")
            .arg(at(16, 10), at(16, 14), at(2, 20), at(16, 48), at(48, 48), at(80, 48));
    };
    const QString contained("#000000 #000000 #00ff00 #00ff00 #008000 #000000");
    QTRY_COMPARE(sample(), QString("#000000 #00ff00 #00ff00 #00ff00 #008000 #000000"));
    const auto uploads = Imv::textureUploads();
    const auto decodes = Imv::imagePipeline().submissions();
    item.setHeight(96); // Within decode hysteresis; layout still changes.
    QTRY_COMPARE(sample(), contained);
    QCOMPARE(Imv::textureUploads(), uploads);
    item.setMask(Imv::ImvImage::Circle);
    QTRY_COMPARE(sample(), QString("#000000 #000000 #000000 #00ff00 #008000 #000000"));
    item.setMask(Imv::ImvImage::NoMask);
    QTRY_COMPARE(sample(), contained);
    QCOMPARE(Imv::imagePipeline().submissions(), decodes);
    item.setTint(Qt::transparent);
    QTRY_COMPARE(sample(), QString("#000000 #000000 #ff0000 #ff0000 #800000 #000000"));
    qInfo() << "graphics API" << window.rendererInterface()->graphicsApi() << "effective DPR"
            << window.effectiveDevicePixelRatio();
}

void ItemTest::firstFramePreview() {
    QQuickWindow window;
    window.resize(100, 80);
    Imv::ImvImage item(window.contentItem());
    item.setSize({64, 32});
    item.setAnimate(false);
    item.setPlaying(false);
    const auto url = QUrl::fromLocalFile(IMV_FIXTURES "/animation.gif");
    window.show();
    QTRY_VERIFY(window.isExposed());
    const qreal dpr = window.effectiveDevicePixelRatio();
    const Imv::DecodeSpec spec{Imv::DecodeSpec::Pixels, {qCeil(64 * dpr), qCeil(32 * dpr)}};
    auto &pipeline = Imv::imagePipeline();
    Imv::Asset first;
    auto request = pipeline.request(Imv::openSource(url), spec,
                                    [&](Imv::Asset asset, Imv::Failure) { first = asset; });
    QTRY_VERIFY(first);
    QVERIFY(!first->animation.count && !first->stream);
    const auto submissions = pipeline.submissions();
    item.setSource(url);
    QTRY_COMPARE(item.status(), Imv::ImvImage::Ready);
    QCOMPARE(pipeline.submissions(), submissions);
    const auto color = [&] {
        const auto frame = window.grabWindow();
        return frame.isNull() ? QColor()
                              : frame.pixelColor(16 * frame.width() / window.width(),
                                                 16 * frame.height() / window.height());
    };
    QTRY_COMPARE(color(), QColor(Qt::red));
    item.setAnimate(true);
    QTRY_COMPARE(item.status(), Imv::ImvImage::Ready);
    QCOMPARE(pipeline.submissions(), submissions + 1);
    QCOMPARE(color(), QColor(Qt::red));
    item.setPlaying(true);
    QTRY_COMPARE(color(), QColor(Qt::green));
}

void ItemTest::animation() {
    QQuickWindow window;
    window.resize(320, 200);
    Imv::ImvImage item(window.contentItem());
    item.setSize({64, 32});
    item.setPlaying(false);
    item.setSource(QUrl::fromLocalFile(IMV_FIXTURES "/animation.gif"));
    window.show();
    QTRY_COMPARE(item.status(), Imv::ImvImage::Ready);
    auto color = [&] {
        const QImage frame = window.grabWindow();
        return frame.isNull() ? QColor() : frame.pixelColor(16, 16);
    };
    QCOMPARE(color(), QColor(Qt::red));
    QTest::qWait(350);
    QCOMPARE(color(), QColor(Qt::red));
    item.setPlaying(true);
    item.setVisible(false);
    QTest::qWait(450);
    item.setVisible(true);
    for (int cycle = 0; cycle < 2; ++cycle) {
        QTRY_COMPARE_WITH_TIMEOUT(color(), QColor(Qt::red), 900);
        QTRY_COMPARE_WITH_TIMEOUT(color(), QColor(Qt::green), 500);
        QTRY_COMPARE_WITH_TIMEOUT(color(), QColor(Qt::yellow), 600);
    }
    QTest::qWait(900);
    QCOMPARE(color(), QColor(Qt::yellow));
}

void ItemTest::cachePolicy() {
    QQmlEngine engine;
    engine.addImportPath(IMV_IMPORT_PATH);
    QQmlComponent component(&engine);
    component.setData(R"(
import QtQml
import ImvQml
QtObject { property bool enabled: ImvCache.enabled; property double budget: ImvCache.byteBudget }
)",
                      QUrl());
    QTRY_VERIFY(!component.isLoading());
    QVERIFY2(component.isReady(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY(object);
    QCOMPARE(object->property("enabled"), false);
    QCOMPARE(object->property("budget").toLongLong(), 512ll * 1024 * 1024);
}

QTEST_MAIN(ItemTest)
#include "item-test.moc"
