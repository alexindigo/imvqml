// SPDX-License-Identifier: LGPL-3.0-only
#include "image-pipeline.h"
#include "playback.h"
#include <QtTest>

using namespace Imv;

class PipelineTest : public QObject {
    Q_OBJECT
private slots:
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
