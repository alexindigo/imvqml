// SPDX-License-Identifier: LGPL-3.0-only
#include "decode-client.h"
#include "helper.h"
#include <QProcess>
#include <QtTest>
#include <cerrno>
#include <imvqml/imvimagejob.h>
#include <poll.h>
#include <sys/syscall.h>
#include <unistd.h>

class VideoTest : public QObject {
    Q_OBJECT
private slots:
    void watchdogWithoutGui() {
        using namespace Imv;
        QProcess process;
        SeqPacket wire(startChild(process, decoderPath()));
        QVERIFY(process.waitForStarted(2000));
        Fd pidfd(int(::syscall(SYS_pidfd_open, process.processId(), 0)));
        QVERIFY(pidfd);
        auto source = openSource(QUrl::fromLocalFile(IMV_FIXTURES "/seek.webm"));
        QVERIFY(source.fd);
        Frame frame{Kind::Request, 1, encode(DecodeSpec{}), {}};
        frame.fds.push_back(std::move(source.fd));
        QCOMPARE(wire.send(frame), Io::Ok);
        // No Qt GUI/pool events are dispatched during this wait.
        struct pollfd exit{pidfd.get(), POLLIN, 0};
        const int retired = ::poll(&exit, 1, StillDeadlineMs + 1500);
        if (retired != 1)
            process.kill();
        QVERIFY(process.waitForFinished(2000));
        QCOMPARE(retired, 1);
        QCOMPARE(process.exitStatus(), QProcess::CrashExit);
    }

    void samples_data() {
        QTest::addColumn<QString>("file");
        QTest::addColumn<double>("position");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QString>("quadrants");
        QTest::newRow("first") << QString("seek.webm") << 0.0 << QSize(128, 72) << QString("rrrr");
        QTest::newRow("seek") << QString("seek.webm") << 0.75 << QSize(128, 72) << QString("bbbb");
        QTest::newRow("large sparse file without extension")
            << QString("large-video") << 0.5 << QSize(128, 72) << QString("rgby");
        QTest::newRow("90") << QString("rotation-90.mp4") << 0.0 << QSize(72, 128)
                            << QString("gyrb");
        QTest::newRow("180") << QString("rotation-180.mp4") << 0.0 << QSize(128, 72)
                             << QString("ybgr");
        QTest::newRow("270") << QString("rotation-270.mp4") << 0.0 << QSize(72, 128)
                             << QString("bryg");
    }

    void samples() {
        QFETCH(QString, file);
        QFETCH(double, position);
        QFETCH(QSize, size);
        QFETCH(QString, quadrants);
        Imv::ImvImageJob job;
        QSignalSpy done(&job, &Imv::ImvImageJob::finished);
        QVERIFY(job.sample(QUrl::fromLocalFile(QStringLiteral(IMV_FIXTURES "/") + file), 128, 128,
                           position) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 9000);
        const auto result = done.first().at(1).toMap();
        QVERIFY2(result.value("code").toString().isEmpty(),
                 qPrintable(result.value("detail").toString()));
        QVERIFY(result.value("detail").toString().isEmpty());
        const auto rgba = result.value("rgba").toByteArray();
        QCOMPARE(result.value("width").toInt(), size.width());
        QCOMPARE(result.value("height").toInt(), size.height());
        QCOMPARE(rgba.size(), size.width() * size.height() * 4);
        for (int i = 0; i < 4; ++i) {
            const auto x = size.width() * (i % 2 ? 3 : 1) / 4;
            const auto y = size.height() * (i / 2 ? 3 : 1) / 4;
            const auto *p =
                reinterpret_cast<const uchar *>(rgba.constData()) + (y * size.width() + x) * 4;
            const auto expected = quadrants.at(i);
            QCOMPARE(p[0] > 128, expected == 'r' || expected == 'y');
            QCOMPARE(p[1] > 128, expected == 'g' || expected == 'y');
            QCOMPARE(p[2] > 128, expected == 'b');
            QCOMPARE(p[3], 255);
        }
    }

    void containment() {
        const auto mode = qgetenv("IMV_VIDEO_TEST");
        if (mode.isEmpty())
            QSKIP("Run by the missing-capability and injected-native-fault tests");
        Imv::ImvImageJob job;
        QSignalSpy done(&job, &Imv::ImvImageJob::finished);
        QVERIFY(job.sample(QUrl::fromLocalFile(IMV_FIXTURES "/seek.webm"), 128, 128) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 9000);
        const auto video = done.takeFirst().at(1).toMap();
        const auto detail = video.value("detail").toString();
        if (mode == "diagnostics") {
            QCOMPARE(video.value("stage").toString(), "decode");
            QCOMPARE(video.value("code").toString(), "invalid-image");
            QVERIFY(detail.contains("native=mpv_command_async(loadfile)"));
            QVERIFY(detail.contains("mpv=-20 (injected failure)"));
            QVERIFY(detail.contains(QStringLiteral("errno=%1").arg(ENOMEM)));
            QVERIFY(QRegularExpression("VmPeak=[1-9][0-9]* kB").match(detail).hasMatch());
            QVERIFY(!detail.contains("warning-1"));
            QVERIFY(!detail.contains("warning-2"));
            const auto lines = detail.split('\n');
            QCOMPARE(lines.size(), 6);
            for (int i = 1; i < lines.size(); ++i) {
                QVERIFY(lines[i].startsWith(QStringLiteral("fake/warn: warning-%1 ").arg(i + 2)));
                QCOMPARE(lines[i].size(), 119);
            }
        } else if (mode != "fault") {
            QCOMPARE(video.value("stage").toString(), "inspect");
            QCOMPARE(video.value("code").toString(), "unsupported-format");
            if (mode == "missing")
                QCOMPARE(detail, "video-support-unavailable");
            else {
                const QByteArray call = mode == "options"
                                            ? "mpv_set_option_string(access-references)"
                                        : mode == "initialize" ? "mpv_initialize"
                                                               : "mpv_render_context_create";
                QVERIFY2(detail.startsWith("video-support-unavailable; native=" + call),
                         qPrintable(detail));
            }
        } else {
            QCOMPARE(video.value("stage").toString(), "helper");
            QCOMPARE(video.value("code").toString(), "helper-crashed");
        }
        int stillId = 0;
        QTRY_VERIFY(
            stillId > 0 ||
            (stillId = job.sample(QUrl::fromLocalFile(IMV_FIXTURES "/palette.png"), 64, 32)) > 0);
        QTRY_COMPARE(done.size(), 1);
        const auto image = done.first().at(1).toMap();
        QCOMPARE(image.value("stage").toString(), "complete");
        QCOMPARE(image.value("rgba").toByteArray().left(4), QByteArray::fromHex("ff0000ff"));
    }

    void invalidMedia() {
        Imv::ImvImageJob job;
        QSignalSpy done(&job, &Imv::ImvImageJob::finished);
        // Matroska magic but no playable stream. The real backend has
        // already started before the media transaction fails.
        QVERIFY(job.sample(QUrl("data:;base64,GkXfowAAAAAAAAAAAAAAAAAAAAA="), 128, 128) > 0);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 9000);
        const auto result = done.first().at(1).toMap();
        QCOMPARE(result.value("stage").toString(), "decode");
        QCOMPARE(result.value("code").toString(), "invalid-image");
        QVERIFY2(result.value("detail").toString().contains("MPV_EVENT_END_FILE(loadfile)"),
                 qPrintable(result.value("detail").toString()));
    }
};

QTEST_GUILESS_MAIN(VideoTest)
#include "video-test.moc"
