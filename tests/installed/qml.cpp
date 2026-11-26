// SPDX-License-Identifier: LGPL-3.0-only
#include <QCoreApplication>
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
    const QByteArray mode(argv[3]);
    const bool pixels = mode == "ON" || mode == "SCAFFOLD";
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
    QTimer::singleShot(4000, &app, [&] { app.exit(3); });
    component.setData(pixels ? R"(
        import QtQml
        import ImvQml
        ImvImageJob {
            required property url fixture
            property int ticket: 0
            property string outcomeStage
            property string outcomeCode
            property string outcomeDetail
            property int sampleWidth
            property int sampleHeight
            property string rgbaHex
            Component.onCompleted: ticket = sample(fixture, 2, 1)
            onFinished: (id, value) => {
                if (id !== ticket) return
                outcomeStage = value.stage ?? ""
                outcomeCode = value.code ?? ""
                outcomeDetail = value.detail ?? ""
                sampleWidth = value.width ?? 0
                sampleHeight = value.height ?? 0
                if (value.rgba !== undefined) {
                    for (const byte of new Uint8Array(value.rgba))
                        rgbaHex += ("0" + byte.toString(16)).slice(-2)
                }
                Qt.quit()
            }
        }
    )"
                             : "import ImvQml\nImvImageJob {}",
                      QUrl());
    std::unique_ptr<QObject> object(
        pixels ? component.createWithInitialProperties(
                     {{"fixture", QUrl::fromLocalFile(fixture.fileName())}})
               : component.create());
    if (!object) {
        qCritical().noquote() << component.errorString();
        return 4;
    }
    QFile mappings("/proc/self/maps");
    if (!mappings.open(QIODevice::ReadOnly))
        return 5;
    // Read line by line: proc pseudo-files advertise a size of zero.
    bool library = false, plugin = false;
    for (;;) {
        const auto line = mappings.readLine();
        if (line.isEmpty())
            break;
        if (!line.contains(prefix + '/'))
            continue;
        library |= line.contains("/libimvqml.so.0.1.0");
        plugin |= line.contains("/libimvqmlplugin.so");
    }
    if (!library || !plugin) {
        qCritical("QML did not map the relocated plugin and backing library");
        return 6;
    }
    if (!pixels)
        return 0;
    if (object->property("ticket").toInt() <= 0)
        return 7;
    QObject::connect(&engine, &QQmlEngine::quit, &app, &QCoreApplication::quit);
    if (const int rc = app.exec(); rc != 0)
        return rc;
    if (mode == "SCAFFOLD") {
        return object->property("outcomeStage").toString() == "decode" &&
                       object->property("outcomeCode").toString() == "not-implemented" &&
                       !object->property("outcomeDetail").toString().isEmpty()
                   ? 0
                   : 9;
    }
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
    return 0;
}
