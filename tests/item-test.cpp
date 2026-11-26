// SPDX-License-Identifier: LGPL-3.0-only
#include "image-node.h"
#include <QColor>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest>

class ItemTest : public QObject {
    Q_OBJECT
private slots:
    void display();
    void animation();
    void cachePolicy();
};

class TestProvider : public QQuickImageProvider {
public:
    TestProvider() : QQuickImageProvider(Image) {}
    QImage requestImage(const QString &id, QSize *original, const QSize &requested) override {
        if (id != "yellow")
            return {};
        *original = requested;
        QImage image(requested, QImage::Format_RGBA8888);
        image.fill(Qt::yellow);
        return image;
    }
};

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
