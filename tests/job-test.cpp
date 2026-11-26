// SPDX-License-Identifier: LGPL-3.0-only
#include "export.h"
#include <imvqml/imvimagejob.h>

#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#include <limits>

namespace {
using Imv::ImvImageJob;

struct ExportGate {
    Imv::Publication phase;
    QSemaphore entered, resume;
    QString path;
};
ExportGate *exportGate = nullptr;

void observeStage(Imv::Publication phase, const QString &path) {
    if (phase != exportGate->phase)
        return;
    exportGate->path = path;
    exportGate->entered.release();
    exportGate->resume.acquire();
}

QUrl fixture(QString name) { return QUrl::fromLocalFile(QStringLiteral(IMV_FIXTURES "/") + name); }

QVariantMap result(QSignalSpy &spy, int id) {
    if (id <= 0 || (!spy.size() && !spy.wait(5000)) || spy.size() != 1)
        return {{"code", "test: no single result"}};
    auto entry = spy.takeFirst();
    return entry[0].toInt() == id ? entry[1].toMap() : QVariantMap{{"code", "test: wrong id"}};
}

QImage pixels(const QVariantMap &value) {
    auto rgba = value.value("rgba").toByteArray();
    const auto w = value.value("width").toInt(), h = value.value("height").toInt();
    if (w <= 0 || h <= 0 || rgba.size() != qint64(w) * h * 4)
        return {};
    return QImage(reinterpret_cast<const uchar *>(rgba.constData()), w, h, w * 4,
                  QImage::Format_RGBA8888)
        .copy();
}

class JobTest : public QObject {
    Q_OBJECT
private slots:
    void formats_data() {
        QTest::addColumn<QString>("extension");
        QTest::addColumn<QString>("format");
        QTest::newRow("PNG") << QString("png") << QString("png");
        QTest::newRow("JPEG") << QString("jpg") << QString("jpeg");
        QTest::newRow("GIF") << QString("gif") << QString("gif");
        QTest::newRow("WebP") << QString("webp") << QString("webp");
        QTest::newRow("TIFF") << QString("tiff") << QString("tiff");
        QTest::newRow("SVG") << QString("svg") << QString("svg");
        QTest::newRow("HEIC") << QString("heic") << QString("heif");
        QTest::newRow("AVIF") << QString("avif") << QString("avif");
        QTest::newRow("JPEG XL") << QString("jxl") << QString("jxl");
    }
    void formats() {
        QFETCH(QString, extension);
        QFETCH(QString, format);
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        const auto url = fixture("palette." + extension);
        const int id = job.inspect(url);
        QCOMPARE(spy.size(), 0);
        const auto info = result(spy, id);
        QVERIFY2(info.value("code").toString().isEmpty(),
                 qPrintable(info.value("detail").toString()));
        const auto metadata = info.value("sourceInfo").toMap();
        QCOMPARE(metadata.value("format").toString(), format);
        QCOMPARE(metadata.value("width").toInt(), 64);
        QCOMPARE(metadata.value("height").toInt(), 32);
        QCOMPARE(metadata.value("frames").toInt(), 1);
        const auto sampled = result(spy, job.sample(url, 20, 12));
        QVERIFY2(sampled.value("code").toString().isEmpty(),
                 qPrintable(sampled.value("detail").toString()));
        auto image = pixels(sampled);
        QCOMPARE(image.size(), QSize(20, 10));
        const auto red = image.pixelColor(3, 4), green = image.pixelColor(16, 4);
        QVERIFY(red.red() > 215 && red.green() < 35 && red.blue() < 35 && red.alpha() == 255);
        QVERIFY(green.green() > 215 && green.red() < 35 && green.blue() < 35 &&
                green.alpha() == 255);
    }

    void orientationAlphaAndData() {
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        const auto rotation = result(spy, job.sample(fixture("orientation.jpg"), 20, 20));
        const auto image = pixels(rotation);
        QCOMPARE(image.size(), QSize(10, 20));
        QVERIFY(image.pixelColor(4, 2).red() > 215);
        QVERIFY(image.pixelColor(4, 16).green() > 215);
        QFile input(fixture("alpha.png").toLocalFile());
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto inMemory =
            QUrl::fromEncoded("data:image/png;base64," + input.readAll().toBase64());
        QCOMPARE(result(spy, job.sample(inMemory, 2, 1)).value("rgba").toByteArray().toHex(),
                 "ff00008000ff0040");
    }

    void outcomes_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("stage");
        QTest::addColumn<QString>("code");
        QTest::newRow("PPM blocked")
            << QString("palette.ppm") << QString("inspect") << QString("unsupported-format");
        QTest::newRow("FITS blocked")
            << QString("palette.fits") << QString("inspect") << QString("unsupported-format");
        QTest::newRow("unrecognized")
            << QString("garbage") << QString("inspect") << QString("unsupported-format");
        QTest::newRow("broken header")
            << QString("broken.png") << QString("inspect") << QString("invalid-image");
        QTest::newRow("source bytes")
            << QString("large-source") << QString("admission") << QString("too-large");
        QTest::newRow("header pixels")
            << QString("huge.png") << QString("inspect") << QString("too-large");
        QTest::newRow("not found")
            << QString("missing.jpg") << QString("open") << QString("unreadable");
    }
    void outcomes() {
        QFETCH(QString, name);
        QFETCH(QString, stage);
        QFETCH(QString, code);
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        auto failure = result(spy, job.sample(fixture(name), 20, 20));
        QCOMPARE(failure.value("stage").toString(), stage);
        QCOMPARE(failure.value("code").toString(), code);
        QVERIFY(!failure.value("detail").toString().isEmpty());
        QVERIFY(!pixels(result(spy, job.sample(fixture("palette.png"), 20, 20))).isNull());
    }

    void invalidPosition() {
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        for (const auto position : {-0.01, 1.01, std::numeric_limits<double>::quiet_NaN()}) {
            const auto failure = result(spy, job.sample(fixture("palette.png"), 20, 20, position));
            QCOMPARE(failure.value("stage").toString(), "admission");
            QCOMPARE(failure.value("code").toString(), "invalid-position");
        }
        const auto failure = result(spy, job.sample(fixture("palette.png"), 0, 20));
        QCOMPARE(failure.value("code").toString(), "invalid-size");
    }

    void exportRace_data() {
        QTest::addColumn<QString>("scenario");
        for (const auto &scenario : {"cancel", "publish", "destroy", "identity"})
            QTest::newRow(scenario) << QString(scenario);
    }

    void exportRace() {
        QFETCH(QString, scenario);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        auto job = std::make_unique<ImvImageJob>();
        QSignalSpy spy(job.get(), &ImvImageJob::finished);
        ExportGate gate{scenario == "publish" ? Imv::Publication::Publishing
                                              : Imv::Publication::Staging};
        exportGate = &gate;
        Imv::observeExports(observeStage);
        const auto cleanup = qScopeGuard([&] {
            gate.resume.release();
            QThreadPool::globalInstance()->waitForDone();
            Imv::observeExports(nullptr);
            exportGate = nullptr;
        });
        const auto dest = dir.filePath("out.png");
        const int id = job->render(fixture("palette.png"), 20, 12, QUrl::fromLocalFile(dest));
        QVERIFY(id > 0);
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered.available(), 3500);
        gate.entered.acquire();
        if (scenario == "cancel") {
            QVERIFY(job->cancel(id));
        } else if (scenario == "publish") {
            QVERIFY(!job->cancel(id));
        } else if (scenario == "destroy") {
            job.reset();
        } else {
            QVERIFY(QFile::rename(gate.path, gate.path + ".original"));
            QFile substitute(gate.path);
            QVERIFY(substitute.open(QIODevice::WriteOnly));
            QCOMPARE(substitute.write("not an image"), 12);
        }
        gate.resume.release();
        QVERIFY(QThreadPool::globalInstance()->waitForDone(4000));
        if (scenario == "destroy") {
            QCoreApplication::processEvents();
            QVERIFY(spy.isEmpty());
        } else {
            const auto value = result(spy, id);
            QCOMPARE(value.value("code").toString(),
                     scenario == "cancel" ? QString("cancelled") : QString());
            QCoreApplication::processEvents();
            QVERIFY(spy.isEmpty());
        }
        if (scenario == "publish" || scenario == "identity") {
            const QImage image(dest);
            QCOMPARE(image.size(), QSize(20, 10));
            QVERIFY(image.pixelColor(3, 4).red() > 215);
            QVERIFY(QFile::remove(dest));
        }
        QCOMPARE(QDir(dir.path())
                     .entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot)
                     .size(),
                 0);
    }

    void publication() {
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        QTemporaryDir dir(QDir::tempPath() + "/imv-publication-XXXXXX");
        QVERIFY(dir.isValid());
        for (const auto &format : {QString("png"), QString("webp")}) {
            auto dest = QUrl::fromLocalFile(dir.filePath("out." + format));
            auto ok = result(spy, job.render(fixture("palette.png"), 20, 12, dest, format));
            QVERIFY2(ok.value("code").toString().isEmpty(),
                     qPrintable(ok.value("detail").toString()));
            QFile file(dest.toLocalFile());
            QVERIFY(file.open(QIODevice::ReadOnly));
            auto original = file.readAll();
            QVERIFY(original.startsWith(format == "png" ? "\x89PNG" : "RIFF"));
            const auto fromDisk = result(spy, job.sample(dest, 20, 12));
            QCOMPARE(pixels(fromDisk).size(), QSize(20, 10));
            QVERIFY(pixels(fromDisk).pixelColor(3, 4).red() > 215);
            auto collision = result(spy, job.render(fixture("palette.png"), 1, 1, dest, format));
            QCOMPARE(collision.value("stage").toString(), "store");
            QCOMPARE(collision.value("code").toString(), "already-exists");
            QVERIFY(file.seek(0));
            QCOMPARE(file.readAll(), original);
        }
        const auto dest = QUrl::fromLocalFile(dir.filePath("cancelled.png"));
        const int ticket = job.render(fixture("palette.png"), 20, 12, dest);
        QVERIFY(job.cancel(ticket));
        QCOMPARE(result(spy, ticket).value("code").toString(), "cancelled");
        QVERIFY(!QFile::exists(dest.toLocalFile()));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 2);
    }
};
} // namespace

QTEST_GUILESS_MAIN(JobTest)
#include "job-test.moc"
