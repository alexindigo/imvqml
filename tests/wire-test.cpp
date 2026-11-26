// SPDX-License-Identifier: LGPL-3.0-only
#include "helper.h"
#include "wire.h"

#include <QProcess>
#include <QtTest>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace Imv;

class MockTransport : public Transport {
public:
    Io send(const Frame &frame) override {
        reply = {Kind::ProbeReply, frame.serial, frame.body, {}};
        return Io::Ok;
    }
    Io receive(Frame &frame) override {
        if (!reply.serial)
            return Io::Again;
        frame = std::exchange(reply, {});
        return Io::Ok;
    }

private:
    Frame reply;
};

void checkProbe(Transport &transport, const Frame &request, Frame &reply) {
    QCOMPARE(transport.send(request), Io::Ok);
    Io received = Io::Again;
    QTRY_VERIFY_WITH_TIMEOUT(
        received != Io::Again || (received = transport.receive(reply)) != Io::Again, 2000);
    QCOMPARE(received, Io::Ok);
    QCOMPARE(reply.kind, Kind::ProbeReply);
    QCOMPARE(reply.serial, request.serial);
    QCOMPARE(reply.body, request.body);
}

class WireTest : public QObject {
    Q_OBJECT
private slots:
    void mockRoundTrip() {
        MockTransport fake;
        Frame reply;
        QCOMPARE(fake.receive(reply), Io::Again);
        checkProbe(fake, {Kind::Probe, 70, QByteArray("m\0ck", 4), {}}, reply);
        QCOMPARE(fake.receive(reply), Io::Again);
    }

    void descriptorRoundTrip() {
        QProcess process;
        {
            auto fd = startChild(process, decoderPath());
            QVERIFY(fd);
            SeqPacket transport(std::move(fd));
            QVERIFY2(process.waitForStarted(2000), qPrintable(process.errorString()));
            int pipes[2];
            QCOMPARE(::pipe2(pipes, O_CLOEXEC), 0);
            Fd writer(pipes[1]);
            QCOMPARE(::write(writer.get(), "x", 1), 1);
            writer = {};
            Frame request{Kind::Probe, 0x1fedcba9876, QByteArray("he\0lo", 5), {}};
            request.fds.emplace_back(pipes[0]);
            Frame reply;
            checkProbe(transport, request, reply);
            QCOMPARE(reply.fds.size(), 1);
            QVERIFY(::fcntl(reply.fds[0].get(), F_GETFD) & FD_CLOEXEC);
            char value = 0;
            QCOMPARE(::read(reply.fds[0].get(), &value, 1), 1);
            QCOMPARE(value, 'x');
        }
        QVERIFY(process.waitForFinished(2000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
    }

    void rejectsBadPackets() {
        auto [a, b] = socketPair();
        QVERIFY(a && b);
        SeqPacket transport(std::move(a));
        Frame frame;
        QCOMPARE(::send(b.get(), "bad", 3, MSG_NOSIGNAL), 3);
        QCOMPARE(transport.receive(frame), Io::Protocol);
        QVERIFY(frame.fds.empty());
        QCOMPARE(transport.send({Kind::Probe, 1, QByteArray(MaxBody + 1, 'x'), {}}), Io::Protocol);
        QCOMPARE(transport.send({Kind::Probe, 0, {}, {}}), Io::Protocol);
        b = {};
        QCOMPARE(transport.receive(frame), Io::Closed);
        QCOMPARE(transport.send({Kind::Probe, 1, {}, {}}), Io::Closed);
    }
};

} // namespace
QTEST_GUILESS_MAIN(WireTest)
#include "wire-test.moc"
