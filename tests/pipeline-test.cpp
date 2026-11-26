// SPDX-License-Identifier: LGPL-3.0-only
#include "image-pipeline.h"
#include <QtTest>

using namespace Imv;

class PipelineTest : public QObject {
    Q_OBJECT
private slots:
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
