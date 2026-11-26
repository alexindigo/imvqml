// SPDX-License-Identifier: LGPL-3.0-only
#include "image-pipeline.h"
#include "playback.h"
#include <QtTest>
#include <fcntl.h>
#include <sys/stat.h>

using namespace Imv;

class PixelChild : public Child {
public:
    void start() override {}
    void terminate() override { reportExit(); }
    Io send(const Frame &frame) override {
        serial = frame.serial;
        count = frame.fds.size();
        if (count == 2) {
            const auto fd = frame.fds[1].get();
            struct stat info{};
            if (::fstat(fd, &info) != 0)
                return Io::Error;
            length = info.st_size;
            seals = ::fcntl(fd, F_GET_SEALS);
        }
        emit readable();
        return Io::Ok;
    }
    Io receive(Frame &frame) override {
        frame = {Kind::Reply, serial, encode(DecodeReport{DecodeCode::Unsupported}), {}};
        return Io::Ok;
    }
    quint64 serial = 0;
    std::size_t count = 0;
    qint64 length = -1;
    int seals = -1;
};

class PipelineTest : public QObject {
    Q_OBJECT
private slots:
    void boundedOutput() {
        PixelChild *child = nullptr;
        Pool pool(
            [&] {
                auto process = std::make_unique<PixelChild>();
                child = process.get();
                return process;
            },
            1, 32);
        int completed = 0;
        for (const auto mode : {DecodeSpec::Inspect, DecodeSpec::Pixels, DecodeSpec::Animation,
                                DecodeSpec::FrameAt}) {
            const DecodeSpec spec{mode, mode == DecodeSpec::Inspect ? QSize(0, 0) : QSize(8, 4)};
            const auto ticket = submitDecode(pool, openSource(QByteArray("test")).fd, spec,
                                             [&](Decoded) { ++completed; });
            QCOMPARE(ticket.status, Pool::Admission::Accepted);
            QTRY_COMPARE(completed, int(mode) + 1);
            QCOMPARE(child->count, mode == DecodeSpec::Inspect ? 1u : 2u);
            if (mode == DecodeSpec::Inspect)
                continue;
            QCOMPARE(child->length, mode == DecodeSpec::Animation ? AnimationBudget : 8 * 4 * 4);
            QCOMPARE(child->seals, F_SEAL_GROW);
        }
    }

    void animationLayouts() {
        Pool pool;
        ImagePipeline pipeline(pool);
        Asset asset;
        Failure failure;
        const auto record = [&](Asset result, Failure error) {
            asset = std::move(result);
            failure = std::move(error);
        };
        for (const auto *path : {IMV_FIXTURES "/animation.gif", IMV_FIXTURES "/animation.webp"}) {
            asset.reset();
            auto interest = pipeline.request(openSource(QUrl::fromLocalFile(path)),
                                             {DecodeSpec::Animation, {64, 32}}, record);
            QTRY_VERIFY(asset || failure);
            QVERIFY2(!failure, qPrintable(failure.detail));
            QCOMPARE(asset->animation.count, 3);
            QCOMPARE(asset->info.loop, 2);
            QCOMPARE(asset->animation.delay(0), 200);
            QCOMPARE(asset->animation.delay(1), 300);
            QCOMPARE(asset->animation.delay(2), 450);
            QColor color = asset->animation.frame(1).pixelColor(16, 16);
            QVERIFY(color.green() > 235 && color.red() < 20 && color.blue() < 20);
        }

        asset.reset();
        auto interest =
            pipeline.request(openSource(QUrl::fromLocalFile(IMV_FIXTURES "/stream.gif")),
                             {DecodeSpec::Animation, {1024, 1024}}, record);
        QTRY_VERIFY(asset || failure);
        QVERIFY2(!failure, qPrintable(failure.detail));
        QVERIFY(asset->stream);
        QCOMPARE(asset->animation.count, 0);
        QCOMPARE(asset->pixels.size(), QSize(1024, 1024));
        QCOMPARE(asset->info.frames, 18);
        QList<QColor> shown;
        Playback playback(asset, [&](Asset frame, Failure error) {
            failure = std::move(error);
            if (frame)
                shown.append(frame->pixels.pixelColor(16, 16));
        });
        playback.run(true);
        QTRY_VERIFY(playback.hasHelper());
        Asset still;
        auto other = pipeline.request(openSource(QUrl::fromLocalFile(IMV_FIXTURES "/palette.png")),
                                      {DecodeSpec::Pixels, {32, 32}},
                                      [&](Asset frame, Failure) { still = std::move(frame); });
        QTRY_VERIFY(still);
        QTRY_VERIFY(shown.size() >= 2 || failure);
        QVERIFY2(!failure, qPrintable(failure.detail));
        QCOMPARE(shown[0], QColor(Qt::green));
        QCOMPARE(shown[1], QColor(Qt::yellow));
        playback.run(false);
        const auto paused = shown.size();
        QVERIFY(playback.hasHelper());
        QTRY_VERIFY(!playback.hasHelper());
        QCOMPARE(playback.buffered(), 0);
        QCOMPARE(shown.size(), paused);
        playback.run(true);
        for (int i = 0; i < 7; ++i) {
            QTRY_VERIFY(shown.size() > paused + i || failure);
            QVERIFY2(!failure, qPrintable(failure.detail));
            QVERIFY(playback.buffered() <= Playback::Ahead);
            const QColor colors[]{Qt::green, Qt::yellow, Qt::red};
            QCOMPARE(shown[paused + i], colors[(paused + i) % 3]);
        }
    }

    void retireInterest() {
        Pool pool;
        ImagePipeline pipeline(pool);
        QSignalSpy releases(&pool, &Pool::capacityReleased);
        const auto url = QUrl::fromLocalFile(IMV_FIXTURES "/palette.png");
        const DecodeSpec spec{DecodeSpec::Pixels, {32, 32}};
        int stale = 0;
        Asset delivered;
        Failure failure;
        const auto old = [&](Asset, Failure) { ++stale; };
        const auto current = [&](Asset asset, Failure error) {
            delivered = std::move(asset);
            failure = std::move(error);
        };

        auto first = pipeline.request(openSource(url), spec, old);
        QCoreApplication::sendPostedEvents(&pipeline, QEvent::MetaCall);
        QCOMPARE(pipeline.submissions(), 1u);
        first.reset(); // Last interest after admission. New request must not attach to it.
        auto second = pipeline.request(openSource(url), spec, old);
        auto third = pipeline.request(openSource(url), spec, current);
        QCoreApplication::sendPostedEvents(&pipeline, QEvent::MetaCall);
        QCOMPARE(pipeline.submissions(), 2u);
        second.reset(); // One of two listeners drops, but remaining interest must survive.
        QTRY_COMPARE(releases.size(), 2);
        QCOMPARE(stale, 0);
        QVERIFY(!failure);
        QVERIFY(delivered);
        QCOMPARE(delivered->pixels.size(), QSize(32, 16));
        QCOMPARE(delivered->pixels.pixelColor(8, 8), QColor(Qt::red));
    }
};

QTEST_GUILESS_MAIN(PipelineTest)
#include "pipeline-test.moc"
