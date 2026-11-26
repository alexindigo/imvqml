// SPDX-License-Identifier: LGPL-3.0-only
#include "probe.h"
#include <imvqml/imvimagejob.h>

#include <QCoreApplication>
#include <QTimer>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 3 || qEnvironmentVariableIsSet("DISPLAY") ||
        qEnvironmentVariableIsSet("WAYLAND_DISPLAY") ||
        !mappedFrom(QFile::decodeName(argv[1]).toUtf8(), false))
        return 1;
    QFile fixture(QFile::decodeName(argv[2]));
    if (!fixture.open(QIODevice::WriteOnly))
        return 2;
    fixture.write("<svg xmlns='http://www.w3.org/2000/svg' width='2' height='1'>"
                  "<path fill='#f00' d='M0 0h1v1H0z'/><path fill='#0f0' d='M1 0h1v1H1z'/></svg>");
    fixture.close();
    Imv::ImvImageJob job;
    const int ticket = job.sample(QUrl::fromLocalFile(fixture.fileName()), 2, 1);
    if (ticket <= 0)
        return 3;
    QObject::connect(
        &job, &Imv::ImvImageJob::finished, &app, [&](int id, const QVariantMap &result) {
            if (id != ticket || result.value("stage") != "complete" ||
                result.value("rgba").toByteArray() != QByteArray::fromHex("ff0000ff00ff00ff"))
                app.exit(4);
            else
                app.quit();
        });
    QTimer::singleShot(10000, &app, [&] { app.exit(5); });
    return app.exec();
}
