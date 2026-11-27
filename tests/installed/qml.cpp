// SPDX-License-Identifier: LGPL-3.0-only
#include "probe.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <memory>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 4 || qEnvironmentVariableIsSet("DISPLAY") ||
        qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))
        return 1;
    QQmlEngine engine;
    engine.addImportPath(QFile::decodeName(argv[1]));
    QQmlComponent component(&engine);
    const bool pixels = QByteArrayView(argv[3]) == "ON";
    auto prefix = QFile::decodeName(argv[2]).toUtf8();
    // No link to imvqml: QML must discover the dynamic plugin.
    QTemporaryDir fixtures(QDir::tempPath() + "/imv-fixture-XXXXXX");
    QFile fixture(fixtures.filePath("palette.svg"));
    if (!fixtures.isValid() || !fixture.open(QIODevice::WriteOnly) ||
        fixture.write(
            "<svg xmlns='http://www.w3.org/2000/svg' width='2' height='1'>"
            "<path fill='#f00' d='M0 0h1v1H0z'/><path fill='#0f0' d='M1 0h1v1H1z'/></svg>") <= 0)
        return 2;
    fixture.close();
    if (!fixture.open(QIODevice::ReadOnly))
        return 2;
    const auto original = fixture.readAll();
    fixture.close();
    QTimer::singleShot(4000, &app, [&] { app.exit(3); });
    component.setData(pixels ? R"(
        import QtQml
        import ImvQml
        ImvImageJob {
            required property url fixture
            required property url destinationDirectory
            property int ticket: 0
            property int retainedTicket: 0
            property string outcomeStage
            property string outcomeCode
            property string outcomeDetail
            property int sampleWidth
            property int sampleHeight
            property string rgbaHex
            property var retained
            Component.onCompleted: {
                ticket = sample(fixture, 2, 1)
                retainedTicket = retainOriginal(fixture, destinationDirectory)
            }
            onFinished: (id, value) => {
                if (id === retainedTicket) {
                    retained = value
                } else if (id === ticket) {
                    outcomeStage = value.stage ?? ""
                    outcomeCode = value.code ?? ""
                    outcomeDetail = value.detail ?? ""
                    sampleWidth = value.width ?? 0
                    sampleHeight = value.height ?? 0
                    if (value.rgba !== undefined) {
                        for (const byte of new Uint8Array(value.rgba))
                            rgbaHex += ("0" + byte.toString(16)).slice(-2)
                    }
                }
                if (retained !== undefined && outcomeStage !== "") Qt.quit()
            }
        }
    )"
                             : "import ImvQml\nImvImageJob {}",
                      QUrl());
    std::unique_ptr<QObject> object(
        pixels ? component.createWithInitialProperties(
                     {{"fixture", QUrl::fromLocalFile(fixture.fileName())},
                      {"destinationDirectory", QUrl::fromLocalFile(fixtures.path())}})
               : component.create());
    if (!object) {
        qCritical().noquote() << component.errorString();
        return 4;
    }
    if (!mappedFrom(prefix, true)) {
        qCritical("QML did not map the relocated plugin and backing library");
        return 6;
    }
    if (!pixels)
        return 0;
    if (object->property("ticket").toInt() <= 0 ||
        object->property("retainedTicket").toInt() <= 0 || !fixture.remove())
        return 7;
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QCoreApplication::quit);
    if (const int rc = app.exec(); rc != 0)
        return rc;
    if (!object->property("outcomeCode").toString().isEmpty() ||
        object->property("sampleWidth").toInt() != 2 ||
        object->property("sampleHeight").toInt() != 1 ||
        object->property("rgbaHex").toString() != "ff0000ff00ff00ff") {
        std::fprintf(stderr,
                     "Installed sample did not provide the expected RGBA pixels: %s: %s: %s\n",
                     qPrintable(object->property("outcomeStage").toString()),
                     qPrintable(object->property("outcomeCode").toString()),
                     qPrintable(object->property("outcomeDetail").toString()));
        return 8;
    }
    const auto retained = object->property("retained").toMap();
    QFile retainedFile(retained.value("path").toString());
    if (retained.value("stage").toString() != "complete" ||
        retained.value("format").toString() != "svg" ||
        retained.value("size").toLongLong() != original.size() ||
        retained.value("sha256").toByteArray() !=
            QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex() ||
        !retainedFile.open(QIODevice::ReadOnly) || retainedFile.readAll() != original) {
        qCritical() << "Installed retention did not preserve the unlinked SVG:" << retained;
        return 9;
    }
    return 0;
}
