// SPDX-License-Identifier: LGPL-3.0-only
#include "export.h"
#include "image-pipeline.h"
#include <imvqml/imvimagejob.h>

#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

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

int retain(ImvImageJob &job, const QUrl &source, const QString &directory) {
    // Exercise the same invokable that QML sees, including on the pre-API tree.
    const auto method = job.metaObject()->indexOfMethod("retainOriginal(QUrl,QUrl)");
    if (method < 0)
        return 0;
    int ticket = 0;
    job.metaObject()->method(method).invoke(&job, Qt::DirectConnection, Q_RETURN_ARG(int, ticket),
                                            Q_ARG(QUrl, source),
                                            Q_ARG(QUrl, QUrl::fromLocalFile(directory)));
    return ticket;
}

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
        const std::pair<const char *, const char *> extra[] = {
            {"jpeg", "jpeg"}, {"tif", "tiff"}, {"heif", "heif"}, {"svgz", "svg"}, {"bmp", "bmp"},
            {"pnm", "ppm"},   {"ppm", "ppm"},  {"pgm", "ppm"},   {"pbm", "ppm"},  {"ico", "ico"},
            {"icns", "icns"}, {"jp2", "jp2"},  {"j2k", "jp2"},   {"exr", "exr"},  {"hdr", "hdr"},
            {"dds", "dds"},   {"tga", "tga"}};
        for (const auto &[ext, format] : extra)
            QTest::newRow(ext) << QString(ext) << QString(format);
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
        const bool square = extension == "ico" || extension == "icns";
        QCOMPARE(metadata.value("height").toInt(), square ? 64 : 32);
        QCOMPARE(metadata.value("frames").toInt(), 1);
        const auto sampled = result(spy, job.sample(url, 20, 12));
        QVERIFY2(sampled.value("code").toString().isEmpty(),
                 qPrintable(sampled.value("detail").toString()));
        auto image = pixels(sampled);
        QCOMPARE(image.size(), square ? QSize(12, 12) : QSize(20, 10));
        const auto red = image.pixelColor(3, 4), green = image.pixelColor(image.width() - 4, 4);
        if (extension == "pgm" || extension == "pbm") {
            QCOMPARE(red, QColor(Qt::white));
            QCOMPARE(green, QColor(Qt::black));
            return;
        }
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

    void burst() {
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        QFile input(fixture("alpha.png").toLocalFile());
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto data = QUrl::fromEncoded("data:image/png;base64," + input.readAll().toBase64());
        QSet<int> inspect, sample;
        for (int i = 0; i < 10; ++i) {
            const int a = job.inspect(fixture("palette.png")), b = job.sample(data, 2, 1);
            QVERIFY2(a > 0 && b > 0, qPrintable(QString("tickets %1, %2").arg(a).arg(b)));
            inspect.insert(a);
            sample.insert(b);
        }
        QVERIFY(spy.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 20, 6000);
        for (const auto &reply : spy) {
            const auto id = reply[0].toInt();
            const auto value = reply[1].toMap();
            QVERIFY2(value.value("code").toString().isEmpty(),
                     qPrintable(value.value("detail").toString()));
            if (inspect.remove(id))
                QCOMPARE(value.value("sourceInfo").toMap().value("width").toInt(), 64);
            else {
                QVERIFY(sample.remove(id));
                QCOMPARE(value.value("rgba").toByteArray().toHex(), "ff00008000ff0040");
            }
        }
        QVERIFY(inspect.isEmpty() && sample.isEmpty());
    }

    void queuedCancellationAndFileCapture() {
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("input.png");
        QVERIFY(QFile::copy(fixture("alpha.png").toLocalFile(), path));
        for (int i = 0; i < 2; ++i)
            QVERIFY(job.inspect(fixture("palette.png")) > 0);
        const auto url = QUrl::fromLocalFile(path);
        const int kept = job.sample(url, 2, 1);
        const auto destination = QUrl::fromLocalFile(dir.filePath("cancelled.png"));
        const int cancelled = job.render(url, 2, 1, destination);
        QVERIFY(kept > 0 && cancelled > 0);
        QVERIFY(job.cancel(cancelled));
        QVERIFY(!job.cancel(cancelled));
        QVERIFY(QFile::remove(path));
        QVERIFY(QFile::copy(fixture("palette.png").toLocalFile(), path));
        QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 4, 6000);
        int sawKept = 0, sawCancelled = 0;
        for (const auto &reply : spy) {
            const auto value = reply[1].toMap();
            if (reply[0].toInt() == cancelled) {
                ++sawCancelled;
                QCOMPARE(value.value("code").toString(), "cancelled");
            } else {
                QVERIFY2(value.value("code").toString().isEmpty(),
                         qPrintable(value.value("detail").toString()));
                if (reply[0].toInt() == kept) {
                    ++sawKept;
                    QCOMPARE(value.value("rgba").toByteArray().toHex(), "ff00008000ff0040");
                }
            }
        }
        QCOMPARE(sawKept, 1);
        QCOMPARE(sawCancelled, 1);
        QVERIFY(!QFile::exists(destination.toLocalFile()));
    }

    void retainFile_data() {
        QTest::addColumn<bool>("replace");
        QTest::newRow("unlink") << false;
        QTest::newRow("replace") << true;
    }

    void retainFile() {
        QFETCH(bool, replace);
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile fixtureFile(fixture("alpha.png").toLocalFile());
        QVERIFY(fixtureFile.open(QIODevice::ReadOnly));
        const auto expected = fixtureFile.readAll();
        const auto source = dir.filePath("original.jpg"); // Deliberately wrong suffix.
        QVERIFY(fixtureFile.copy(source));
        const int id = retain(job, QUrl::fromLocalFile(source), dir.path());
        QVERIFY(QFile::remove(source));
        if (replace)
            QVERIFY(QFile::copy(fixture("palette.jpg").toLocalFile(), source));
        QVERIFY(id > 0);
        QVERIFY(spy.isEmpty());
        const auto value = result(spy, id);
        QCOMPARE(value.value("stage").toString(), "complete");
        QCOMPARE(value.value("format").toString(), "png");
        QCOMPARE(value.value("size").toLongLong(), expected.size());
        QCOMPARE(value.value("sha256").toString(),
                 QString::fromLatin1(
                     QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex()));
        QFile output(value.value("path").toString());
        QCOMPARE(QFileInfo(output).absolutePath(), dir.path());
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), expected);
    }

    void synchronousRefusal_data() {
        QTest::addColumn<QString>("method");
        QTest::addColumn<QString>("input");
        for (const auto *method : {"inspect", "sample", "render", "retainOriginal"}) {
            for (const auto *input : {"missing", "fifo", "denied", "network"})
                QTest::newRow(QByteArray(method) + '-' + input)
                    << QString(method) << QString(input);
        }
        QTest::newRow("retainOriginal-oversize") << QString("retainOriginal") << QString("large");
    }

    void synchronousRefusal() {
        QFETCH(QString, method);
        QFETCH(QString, input);
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("input");
        auto source = QUrl::fromLocalFile(path);
        if (input == "fifo") {
            QCOMPARE(::mkfifo(QFile::encodeName(path).constData(), 0600), 0);
        } else if (input == "denied") {
            if (::geteuid() == 0)
                QSKIP("mode 000 requires an unprivileged test runner");
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.close();
            QCOMPARE(::chmod(QFile::encodeName(path).constData(), 0000), 0);
            Imv::Fd denied(::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC));
            const auto openError = errno;
            QVERIFY(!denied);
            QCOMPARE(openError, EACCES);
        } else if (input == "large") {
            source = fixture("large-source");
        } else if (input == "network") {
            source = QUrl("https://example.invalid/image.png");
        }
        const auto dest = QUrl::fromLocalFile(dir.filePath("output.png"));
        const int id = method == "inspect"  ? job.inspect(source)
                       : method == "sample" ? job.sample(source, 20, 20)
                       : method == "render" ? job.render(source, 20, 20, dest)
                                            : retain(job, source, dir.path());
        const bool large = input == "large", network = input == "network";
        QCOMPARE(id, large ? -5 : network ? -1 : -4);
        QCOMPARE(job.property("lastErrorStage").toString(),
                 large || network ? "admission" : "open");
        QCOMPARE(job.property("lastErrorCode").toString(), large     ? "too-large"
                                                           : network ? "invalid-source"
                                                                     : "unreadable");
        QVERIFY(!job.property("lastErrorDetail").toString().isEmpty());
        QVERIFY(!job.cancel(id));
        const auto accepted = job.inspect(fixture("palette.png"));
        QCOMPARE(accepted, 1);
        QVERIFY(job.property("lastErrorCode").toString().isEmpty());
        QCOMPARE(result(spy, accepted).value("stage").toString(), "complete");
        QCoreApplication::processEvents();
        QVERIFY(spy.isEmpty());
        QVERIFY(!QFile::exists(dest.toLocalFile()));
    }

    void retainDataAndCancelDuringStaging() {
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile input(fixture("palette.gif").toLocalFile());
        QVERIFY(input.open(QIODevice::ReadOnly));
        const auto bytes = input.readAll();
        const auto source = QUrl::fromEncoded("data:image/gif;base64," + bytes.toBase64());
        const auto value = result(spy, retain(job, source, dir.path()));
        QCOMPARE(value.value("format").toString(), "gif");
        QFile output(value.value("path").toString());
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(output.readAll(), bytes);
        output.close();
        QVERIFY(output.remove());

        ExportGate gate{Imv::Publication::Staging};
        exportGate = &gate;
        Imv::observeExports(observeStage);
        const auto cleanup = qScopeGuard([&] {
            gate.resume.release();
            QThreadPool::globalInstance()->waitForDone();
            Imv::observeExports(nullptr);
            exportGate = nullptr;
        });
        const auto id = retain(job, source, dir.path());
        QVERIFY(id > 0);
        QTRY_VERIFY_WITH_TIMEOUT(gate.entered.available(), 3500);
        QVERIFY(job.cancel(id));
        gate.resume.release();
        QCOMPARE(result(spy, id).value("code").toString(), "cancelled");
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{});
    }

    void retainSharedQueue() {
        ImvImageJob job;
        QSignalSpy spy(&job, &ImvImageJob::finished);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QList<int> tickets;
        for (int i = 0; i < Imv::ImagePipeline::QueueLimit - 1; ++i) {
            const auto ticket = job.inspect(fixture("alpha.png"));
            QVERIFY(ticket > 0);
            tickets.append(ticket);
        }
        const auto id = retain(job, fixture("alpha.png"), dir.path());
        QVERIFY(id > 0);
        QCOMPARE(retain(job, fixture("alpha.png"), dir.path()), -2);
        QCOMPARE(job.inspect(fixture("alpha.png")), -2);
        QVERIFY(job.cancel(id));
        QVERIFY(!job.cancel(id));
        QCOMPARE(result(spy, id).value("code").toString(), "cancelled");
        for (const auto ticket : tickets)
            QVERIFY(job.cancel(ticket));
        QCoreApplication::processEvents();
        QCOMPARE(spy.size(), tickets.size());
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
                 QStringList{});
    }

    void outcomes_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QString>("stage");
        QTest::addColumn<QString>("code");
        QTest::newRow("FITS blocked")
            << QString("palette.fits") << QString("inspect") << QString("unsupported-format");
        QTest::newRow("unrecognized")
            << QString("garbage") << QString("inspect") << QString("unsupported-format");
        QTest::newRow("broken header")
            << QString("broken.png") << QString("inspect") << QString("invalid-image");
        for (const auto *ext : {"bmp", "ico", "icns", "dds", "exr", "hdr", "jp2", "tga", "svgz"})
            QTest::newRow(ext) << (QString("broken.") + ext) << QString("inspect")
                               << QString("invalid-image");
        QTest::newRow("truncated PPM pixels")
            << QString("broken.ppm") << QString("decode") << QString("invalid-image");
        QTest::newRow("source bytes")
            << QString("large-source") << QString("admission") << QString("too-large");
        QTest::newRow("header pixels")
            << QString("huge.png") << QString("inspect") << QString("too-large");
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
            QCOMPARE(job.sample(fixture("palette.png"), 20, 20, position), -1);
            QCOMPARE(job.property("lastErrorStage").toString(), "admission");
            QCOMPARE(job.property("lastErrorCode").toString(), "invalid-position");
        }
        QCOMPARE(job.sample(fixture("palette.png"), 0, 20), -1);
        QCOMPARE(job.property("lastErrorCode").toString(), "invalid-size");
        QCOMPARE(job.render(fixture("palette.png"), 20, 20, QUrl{}), -1);
        QCOMPARE(job.property("lastErrorCode").toString(), "invalid-destination");
        QCoreApplication::processEvents();
        QVERIFY(spy.isEmpty());
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
