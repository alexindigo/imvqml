// SPDX-License-Identifier: LGPL-3.0-only
#include "probe.h"
#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QTimer>

int main(int argc, char **argv) {
    if (argc != 4 || !qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
        qCritical("The relocated visual consumer requires the niri Wayland session");
        return 1;
    }
    QGuiApplication app(argc, argv);
    if (app.platformName() != "wayland")
        return 2;
    QFile fixture(QFile::decodeName(argv[3]));
    if (!fixture.open(QIODevice::WriteOnly))
        return 3;
    fixture.write("<svg xmlns='http://www.w3.org/2000/svg' width='128' height='64'>"
                  "<path fill='#f00' d='M0 0h32v64H0z'/>"
                  "<path fill='#0f0' fill-opacity='0.5' d='M32 0h48v64H32z'/></svg>");
    fixture.close();
    QQmlEngine engine;
    engine.addImportPath(QFile::decodeName(argv[1]));
    QQmlComponent component(&engine);
    component.setData(R"(
        import QtQuick
        import ImvQml
        Window {
            width: 320; height: 200; color: "blue"
            required property url fixture
            property alias status: picture.status
            property alias errorCode: picture.errorCode
            property color foreground: "yellow"
            ImvImage {
                id: picture; width: 128; height: 64; source: fixture
                smooth: false; tint: foreground
            }
        }
    )",
                      QUrl());
    std::unique_ptr<QObject> object(component.createWithInitialProperties(
        {{"fixture", QUrl::fromLocalFile(fixture.fileName())}}));
    auto *window = qobject_cast<QQuickWindow *>(object.get());
    if (!window) {
        qCritical().noquote() << component.errorString();
        return 4;
    }
    if (!mappedFrom(QFile::decodeName(argv[2]).toUtf8(), true)) {
        qCritical("Visual consumer did not map the relocated plugin and backing library");
        return 7;
    }
    bool presented = false;
    QObject::connect(
        window, &QQuickWindow::frameSwapped, window, [&] { presented = true; },
        Qt::QueuedConnection);
    QTimer inspect;
    QObject::connect(&inspect, &QTimer::timeout, &app, [&] {
        if (!window->property("errorCode").toString().isEmpty()) {
            qCritical().noquote() << window->property("errorCode").toString();
            app.exit(5);
            return;
        }
        if (!presented || !window->isExposed())
            return;
        const auto image = window->grabWindow();
        if (image.isNull())
            return;
        const auto scale = qreal(image.width()) / window->width();
        const auto pixel = [&](int x, int y) {
            return image.pixelColor(qRound(x * scale), qRound(y * scale));
        };
        const auto blend = pixel(56, 32);
        if (pixel(16, 32) == QColor(Qt::yellow) && qAbs(blend.red() - 128) <= 2 &&
            qAbs(blend.green() - 128) <= 2 && qAbs(blend.blue() - 127) <= 2 &&
            pixel(96, 32) == QColor(Qt::blue) && pixel(140, 32) == QColor(Qt::blue)) {
            qInfo("Installed QML item presented tinted opaque and half-alpha pixels plus clear "
                  "coverage");
            app.quit();
        }
    });
    QTimer::singleShot(12000, &app, [&] {
        qCritical("Missing relocated display pixels");
        app.exit(6);
    });
    inspect.start(40);
    window->show();
    return app.exec();
}
