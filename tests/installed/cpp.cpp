// SPDX-License-Identifier: LGPL-3.0-only
#include "probe.h"
#include <imvqml/imvimagejob.h>

#include <QCoreApplication>
#include <QTimer>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if ((argc != 3 && argc != 5) || qEnvironmentVariableIsSet("DISPLAY") ||
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
    int ticket = job.sample(QUrl::fromLocalFile(fixture.fileName()), 2, 1);
    if (ticket <= 0)
        return 3;
    bool still = true;
    QObject::connect(
        &job, &Imv::ImvImageJob::finished, &app, [&](int id, const QVariantMap &result) {
            if (id != ticket) {
                app.exit(4);
            } else if (still) {
                if (result.value("stage") != "complete" ||
                    result.value("rgba").toByteArray() != QByteArray::fromHex("ff0000ff00ff00ff")) {
                    app.exit(4);
                } else if (argc == 3) {
                    app.quit();
                } else {
                    still = false;
                    ticket = job.sample(QUrl::fromLocalFile(QFile::decodeName(argv[3])), 128, 72);
                    if (ticket <= 0)
                        app.exit(3);
                }
            } else if (QByteArrayView(argv[4]) == "unavailable") {
                if (result.value("stage") != "inspect" ||
                    result.value("code") != "unsupported-format" ||
                    result.value("detail") != "video-support-unavailable")
                    app.exit(6);
                else
                    app.quit();
            } else {
                const auto rgba = result.value("rgba").toByteArray();
                if (result.value("stage") != "complete" || result.value("width") != 128 ||
                    result.value("height") != 72 || rgba.size() != 128 * 72 * 4 ||
                    uchar(rgba[0]) <= 128 || uchar(rgba[1]) >= 32 || uchar(rgba[2]) >= 32 ||
                    uchar(rgba[3]) != 255)
                    app.exit(7);
                else
                    app.quit();
            }
        });
    QTimer::singleShot(10000, &app, [&] { app.exit(5); });
    return app.exec();
}
