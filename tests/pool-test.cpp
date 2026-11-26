// SPDX-License-Identifier: LGPL-3.0-only
#include "pool.h"

#include <QElapsedTimer>
#include <QtTest>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace {
using namespace Imv;
using namespace std::chrono_literals;

struct Hold {
    Fd read;
    pid_t pid = 0;

    Frame request() {
        Frame work{Kind::Request, 0, "hold", {}};
        int pipes[2];
        if (::pipe2(pipes, O_CLOEXEC | O_NONBLOCK) == 0) {
            read = Fd(pipes[0]);
            work.fds.emplace_back(pipes[1]);
        }
        return work;
    }
    bool started() { return pid > 0 || ::read(read.get(), &pid, sizeof pid) == sizeof pid; }
    bool retired() const { return pid > 0 && ::kill(pid, 0) < 0 && errno == ESRCH; }
};

class FakeChild : public Child {
public:
    void start() override {}
    void terminate() override { retiring = true; }
    void reap() { reportExit(); }
    Io send(const Frame &frame) override {
        serial = frame.serial;
        return Io::Ok;
    }
    Io receive(Frame &frame) override {
        if (!reply)
            return Io::Again;
        reply = false;
        frame = {Kind::Reply, serial, "reply", {}};
        return Io::Ok;
    }
    void answer() {
        reply = true;
        emit readable();
    }
    quint64 serial = 0;
    bool retiring = false;

private:
    bool reply = false;
};

class PoolTest : public QObject {
    Q_OBJECT
    static Pool::Factory native() {
        return [] { return processChild(QStringLiteral(IMV_FAULT_CHILD)); };
    }
private slots:
    void negativeAdmissions() {
        const auto factory = [] { return std::make_unique<FakeChild>(); };
        Pool busy(factory, 0, 32);
        Pool exhausted(factory, 1, 32, nullptr, std::numeric_limits<int>::max());
        const auto submit = [](Pool &pool) {
            return pool.submit({Kind::Request, 0, "test", {}}, 1s, [](JobReply) {});
        };
        QCOMPARE(busy.capacity(), Pool::Admission::Busy);
        QCOMPARE(submit(busy).status, Pool::Admission::Busy);
        QCOMPARE(exhausted.capacity(), Pool::Admission::Exhausted);
        QCOMPARE(submit(exhausted).status, Pool::Admission::Exhausted);
    }

    void killedAndRespawned() {
        Pool pool(native(), 1, 32);
        QSignalSpy releases(&pool, &Pool::capacityReleased);
        QList<QString> results;
        auto record = [&](JobReply reply) { results << reply.error.code; };
        Hold hold;
        const auto request = pool.submit(hold.request(), 2s, record);
        QCOMPARE(request.status, Pool::Admission::Accepted);
        QVERIFY(request.id > 0);
        QVERIFY(results.empty());
        QCOMPARE(pool.submit({Kind::Request, 0, "ok", {}}, 2s, record).status,
                 Pool::Admission::Busy);
        QTRY_VERIFY_WITH_TIMEOUT(hold.started(), 1000);
        QCOMPARE(::kill(hold.pid, SIGKILL), 0);
        QTRY_COMPARE_WITH_TIMEOUT(releases.size(), 1, 2000);
        QVERIFY(hold.retired());
        QCOMPARE(results, QList<QString>{"helper-crashed"});
        const auto recovered = pool.submit({Kind::Request, 0, "ok", {}}, 2s, record);
        QCOMPARE(recovered.status, Pool::Admission::Accepted);
        QVERIFY(recovered.id > request.id);
        QTRY_COMPARE(releases.size(), 2);
        QCOMPARE(results, (QList<QString>{"helper-crashed", ""}));
    }

    void deadlineAndProtocol_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<QString>("code");
        QTest::newRow("deadline") << QByteArray("hold") << QString("deadline");
        QTest::newRow("garbage") << QByteArray("garbage") << QString("protocol-error");
        QTest::newRow("correlation") << QByteArray("wrong-serial") << QString("protocol-error");
    }
    void deadlineAndProtocol() {
        QFETCH(QByteArray, mode);
        QFETCH(QString, code);
        Pool pool(native(), 1, 32);
        QSignalSpy releases(&pool, &Pool::capacityReleased);
        QElapsedTimer elapsed;
        elapsed.start();
        Hold hold;
        JobReply reply;
        int calls = 0;
        const bool hang = mode == "hold";
        const auto ticket = pool.submit(hang ? hold.request() : Frame{Kind::Request, 0, mode, {}},
                                        400ms, [&](JobReply value) {
                                            ++calls;
                                            reply = std::move(value);
                                        });
        QCOMPARE(ticket.status, Pool::Admission::Accepted);
        if (hang)
            QTRY_VERIFY_WITH_TIMEOUT(hold.started(), 300);
        QTRY_COMPARE_WITH_TIMEOUT(releases.size(), 1, 1500);
        QCOMPARE(calls, 1);
        QCOMPARE(reply.error.stage, "helper");
        QCOMPARE(reply.error.code, code);
        if (hang) {
            QVERIFY(elapsed.elapsed() >= 400);
            QVERIFY(hold.retired());
        }
        QVERIFY(!pool.cancel(ticket.id));
        bool ok = false;
        QCOMPARE(pool.submit({Kind::Request, 0, "ok", {}}, 1s,
                             [&](JobReply value) { ok = !value.error; })
                     .status,
                 Pool::Admission::Accepted);
        QTRY_VERIFY(ok);
    }

    void cancellationAndReplyRace() {
        FakeChild *fake = nullptr;
        Pool pool(
            [&] {
                auto child = std::make_unique<FakeChild>();
                fake = child.get();
                return child;
            },
            1, 32);
        QList<QString> sequence;
        connect(&pool, &Pool::capacityReleased, &pool, [&](int) { sequence << "released"; });
        auto record = [&](JobReply reply) {
            sequence << (reply.error ? reply.error.code : "complete");
        };
        const auto id = pool.submit({Kind::Request, 0, {}, {}}, 1s, record).id;
        QVERIFY(id > 0 && fake);
        QCOMPARE(sequence.size(), 0);
        fake->answer(); // Queued socket event; cancellation claims the ledger now.
        QVERIFY(pool.cancel(id));
        QVERIFY(fake->retiring);
        QVERIFY(!pool.available());
        QVERIFY(!pool.cancel(id));
        fake->reap(); // May be queued before cancellation is delivered.
        QTRY_COMPARE(sequence, (QList<QString>{"cancelled", "released"}));
        QVERIFY(pool.available());

        const auto next = pool.submit({Kind::Request, 0, {}, {}}, 1s, record).id;
        QVERIFY(next > id);
        fake->answer();
        QTRY_COMPARE(sequence, (QList<QString>{"cancelled", "released", "complete", "released"}));
        QVERIFY(!pool.cancel(next));
    }

    void reuseLimitAndDestruction() {
        auto pool = std::make_unique<Pool>(native(), 1, 2);
        QSignalSpy releases(pool.get(), &Pool::capacityReleased);
        QList<int> pids;
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(pool->submit({Kind::Request, 0, "ok", {}}, 1s,
                                  [&](JobReply reply) { pids << reply.frame.body.toInt(); })
                         .status,
                     Pool::Admission::Accepted);
            QTRY_COMPARE_WITH_TIMEOUT(releases.size(), i + 1, 1500);
        }
        QVERIFY(pids.at(0) > 0);
        QCOMPARE(pids.at(0), pids.at(1));
        QVERIFY(pids.at(2) > 0 && pids.at(2) != pids.at(0));
        Hold hold;
        bool callback = false;
        QCOMPARE(pool->submit(hold.request(), 1s, [&](JobReply) { callback = true; }).status,
                 Pool::Admission::Accepted);
        QTRY_VERIFY_WITH_TIMEOUT(hold.started(), 800);
        pool.reset();
        QTRY_VERIFY_WITH_TIMEOUT(hold.retired(), 1500);
        QVERIFY(!callback);
    }
};
} // namespace
QTEST_GUILESS_MAIN(PoolTest)
#include "pool-test.moc"
