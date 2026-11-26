// SPDX-License-Identifier: LGPL-3.0-only
#include "image-pipeline.h"
#include "playback.h"
#include <QtTest>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
        if (!held)
            emit readable();
        return Io::Ok;
    }
    Io receive(Frame &frame) override {
        frame = {Kind::Reply,
                 serial,
                 encode(DecodeReport{DecodeCode::Unsupported, {}, {0, 0}, "Fixture"}),
                 {}};
        return Io::Ok;
    }
    quint64 serial = 0;
    std::size_t count = 0;
    qint64 length = -1;
    int seals = -1;
    bool held = false;
    void release() {
        held = false;
        emit readable();
    }
};

class ReportChild : public Child {
public:
    enum Fault { None, ExtraFd, Malformed, Unsealed, Short };
    explicit ReportChild(Fault fault) : fault(fault) {}
    void start() override {}
    void terminate() override { reportExit(); }
    Io send(const Frame &request) override {
        const int fd = request.fds.at(1).get();
        const auto report = DecodeReport{DecodeCode::Ok, {"png", {8, 4}}, {8, 4}};
        if (fault != Unsealed && !sealPixelOutput(fd, report.outputBytes() - (fault == Short)))
            return Io::Error;
        next = {Kind::Reply, request.serial, fault == Malformed ? "invalid" : encode(report), {}};
        if (fault == ExtraFd)
            next.fds.push_back(newMemfd("rejected"));
        emit readable();
        return Io::Ok;
    }
    Io receive(Frame &reply) override {
        reply = std::exchange(next, {});
        return Io::Ok;
    }

private:
    Fault fault;
    Frame next;
};

class PipelineTest : public QObject {
    Q_OBJECT
private slots:
    void backpressure() {
        PixelChild *child = nullptr;
        Pool pool(
            [&] {
                auto process = std::make_unique<PixelChild>();
                process->held = true;
                child = process.get();
                return process;
            },
            1, 32);
        auto blocker = submitDecode(pool, openSource(QByteArray("blocker")).fd, {}, [](Decoded) {});
        QCOMPARE(blocker.status, Pool::Admission::Accepted);
        ImagePipeline pipeline(pool);
        QList<int> delivered;
        QStringList errors;
        const auto record = [&](int id, bool visible) {
            return pipeline.request(
                SourceInput(QByteArray::number(id)), {DecodeSpec::Pixels, {8, 4}},
                [&, id](Asset, Failure error) {
                    delivered.append(id);
                    errors.append(error.code);
                },
                visible);
        };
        auto hidden = record(1, false);
        auto promoted = record(2, false);
        auto visible = record(3, true);
        auto cancelled = record(4, true);
        QCoreApplication::sendPostedEvents(&pipeline, QEvent::MetaCall);
        QCOMPARE(pipeline.submissions(), 0u);
        pipeline.setVisible(promoted, true);
        cancelled.reset();
        child->release();
        QTRY_COMPARE(delivered.size(), 3);
        QCOMPARE(delivered, (QList<int>{2, 3, 1}));
        QCOMPARE(errors,
                 (QStringList{"unsupported-format", "unsupported-format", "unsupported-format"}));
        QCOMPARE(pipeline.submissions(), 3u);
    }

    void queueDeadline() {
        Pool pool(
            [] {
                auto process = std::make_unique<PixelChild>();
                process->held = true;
                return process;
            },
            1, 32);
        QCOMPARE(
            submitDecode(pool, openSource(QByteArray("blocker")).fd, {}, [](Decoded) {}).status,
            Pool::Admission::Accepted);
        QSignalSpy releases(&pool, &Pool::capacityReleased);
        ImagePipeline pipeline(pool, nullptr, 100);
        Failure failure;
        auto interest =
            pipeline.request(SourceInput(QByteArray("wait")), {DecodeSpec::Pixels, {8, 4}},
                             [&](Asset, Failure error) { failure = std::move(error); });
        QTRY_VERIFY(failure);
        QCOMPARE(failure.stage, "admission");
        QCOMPARE(failure.code, "deadline");
        QCOMPARE(releases.size(), 0);
        QCOMPARE(pipeline.submissions(), 0u);
    }

    void explicitQueueDeadline() {
        Pool pool(
            [] {
                auto process = std::make_unique<PixelChild>();
                process->held = true;
                return process;
            },
            1, 32);
        QCOMPARE(
            submitDecode(pool, openSource(QByteArray("blocker")).fd, {}, [](Decoded) {}).status,
            Pool::Admission::Accepted);
        QSignalSpy releases(&pool, &Pool::capacityReleased);
        ImagePipeline pipeline(pool, nullptr, 100);
        int events = 0;
        const auto done = [&](Asset asset, Failure failure) {
            ++events;
            QVERIFY(!asset);
            QCOMPARE(failure.stage, "admission");
            QCOMPARE(failure.code, "deadline");
        };
        const auto spec = DecodeSpec{DecodeSpec::Pixels, {8, 4}};
        auto ready = pipeline.requestExplicit(spec, done);
        QImage pixels(8, 4, QImage::Format_RGBA8888);
        pixels.fill(Qt::white);
        pipeline.captured(ready, ProviderSnapshot{pixels.size(), pixels, {}});
        auto awaiting = pipeline.requestExplicit(spec, done);
        QVERIFY(ready && awaiting);
        QTRY_COMPARE(events, 2);
        QCOMPARE(releases.size(), 0);
        QCOMPARE(pipeline.submissions(), 0u);
        pipeline.captured(awaiting, ProviderSnapshot{pixels.size(), pixels, {}});
        QCoreApplication::processEvents();
        QCOMPARE(events, 2);
    }

    void immutableRevision() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("mutable");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QCOMPARE(file.write("before"), 6);
        QVERIFY(file.flush());
        struct stat original{};
        QCOMPARE(::fstat(file.handle(), &original), 0);
        const auto url = QUrl::fromLocalFile(path);
        auto first = openSource(url);
        QVERIFY(first.fd);
        QTest::qWait(20);
        QVERIFY(file.seek(0));
        QCOMPARE(file.write("rewrit"), 6);
        QVERIFY(file.flush());
        const timespec times[]{{0, UTIME_OMIT}, original.st_mtim};
        QCOMPARE(::futimens(file.handle(), times), 0);
        auto second = openSource(url);
        QVERIFY(second.fd);
        char buffer[6];
        QCOMPARE(::pread(first.fd.get(), buffer, sizeof buffer, 0), 6);
        QCOMPARE(QByteArray(buffer, 6), "before");
        QCOMPARE(::fcntl(first.fd.get(), F_GET_SEALS) & MemfdSeals, MemfdSeals);
        QVERIFY(first.identity != second.identity);
        QCOMPARE(::pread(second.fd.get(), buffer, sizeof buffer, 0), 6);
        QCOMPARE(QByteArray(buffer, 6), "rewrit");
    }

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

    void malformedOutput_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("extra-fd") << int(ReportChild::ExtraFd);
        QTest::newRow("malformed-report") << int(ReportChild::Malformed);
        QTest::newRow("missing-seals") << int(ReportChild::Unsealed);
        QTest::newRow("wrong-size") << int(ReportChild::Short);
    }

    void malformedOutput() {
        QFETCH(int, fault);
        int launches = 0;
        Pool pool(
            [&] {
                return std::make_unique<ReportChild>(launches++ ? ReportChild::None
                                                                : ReportChild::Fault(fault));
            },
            1, 32);
        QSignalSpy releases(&pool, &Pool::capacityReleased);
        QList<Decoded> results;
        const auto submit = [&] {
            return submitDecode(pool, openSource(QByteArray("test")).fd,
                                {DecodeSpec::Pixels, {8, 4}},
                                [&](Decoded image) { results.append(std::move(image)); });
        };
        QCOMPARE(submit().status, Pool::Admission::Accepted);
        QTRY_COMPARE(releases.size(), 1);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].error.stage, "helper");
        QCOMPARE(results[0].error.code, "protocol-error");
        QCOMPARE(submit().status, Pool::Admission::Accepted);
        QTRY_COMPARE(releases.size(), 2);
        QCOMPARE(results.size(), 2);
        QVERIFY(!results[1].error);
        QCOMPARE(results[1].pixels.size(), QSize(8, 4));
        QCOMPARE(launches, 2);
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
            QVERIFY(playback.bufferedBytes() <= AnimationBudget);
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
