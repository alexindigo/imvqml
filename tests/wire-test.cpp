// SPDX-License-Identifier: LGPL-3.0-only
#include "decode-wire.h"
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

    void helperSourceContract() {
        QProcess process;
        auto transport = std::make_unique<SeqPacket>(startChild(process, decoderPath()));
        QVERIFY(process.waitForStarted(2000));
        const QByteArray svg = "<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'/>";
        const auto inspect = [&](quint64 serial, Fd source, DecodeCode expected) {
            Frame request{Kind::Request, serial, encode(DecodeSpec{}), {}};
            request.fds.push_back(std::move(source));
            QCOMPARE(transport->send(request), Io::Ok);
            Frame reply;
            Io status = Io::Again;
            QTRY_VERIFY(status != Io::Again || (status = transport->receive(reply)) != Io::Again);
            QCOMPARE(status, Io::Ok);
            QCOMPARE(reply.serial, serial);
            DecodeReport report;
            QVERIFY(decode(reply.body, report));
            QCOMPARE(report.code, expected);
        };
        auto mutableInput = newMemfd("mutable-source");
        QVERIFY(writeAll(mutableInput.get(), svg.constData(), svg.size()));
        inspect(1, std::move(mutableInput), DecodeCode::Unreadable);
        auto sealed = newMemfd("oversized-source");
        QCOMPARE(::ftruncate(sealed.get(), MaxSourceBytes + 1), 0);
        QCOMPARE(::fcntl(sealed.get(), F_ADD_SEALS, MemfdSeals), 0);
        inspect(2, std::move(sealed), DecodeCode::SourceLimit);
        auto bounded = newMemfd("bounded-source");
        QVERIFY(writeAll(bounded.get(), svg.constData(), svg.size()));
        QCOMPARE(::fcntl(bounded.get(), F_ADD_SEALS, MemfdSeals), 0);
        inspect(3, std::move(bounded), DecodeCode::Ok);
        transport.reset();
        QVERIFY(process.waitForFinished(2000));
    }

    void ancillaryRejection_data() {
        QTest::addColumn<int>("count");
        QTest::newRow("too-many") << 3;
        QTest::newRow("truncated") << 8;
    }

    void ancillaryRejection() {
        QFETCH(int, count);
        auto [a, b] = socketPair();
        SeqPacket transport(std::move(a));
        Fd source(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        QVERIFY(source);
        const auto openCount = [] {
            return QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        };
        const auto before = openCount();
        auto [encoded, drain] = socketPair();
        SeqPacket encoder(std::move(encoded));
        QCOMPARE(encoder.send({Kind::Probe, 1, {}, {}}), Io::Ok);
        char body[64];
        const auto length = ::recv(drain.get(), body, sizeof body, 0);
        QVERIFY(length > 0);
        struct iovec data{body, size_t(length)};
        alignas(struct cmsghdr) char control[CMSG_SPACE(8 * sizeof(int))]{};
        struct msghdr message{};
        message.msg_iov = &data;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = CMSG_SPACE(count * sizeof(int));
        auto *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(count * sizeof(int));
        auto *fds = reinterpret_cast<int *>(CMSG_DATA(header));
        std::fill_n(fds, count, source.get());
        QCOMPARE(::sendmsg(b.get(), &message, MSG_NOSIGNAL), length);
        Frame frame;
        QCOMPARE(transport.receive(frame), Io::Protocol);
        QVERIFY(frame.fds.empty());
        QCOMPARE(openCount(), before + 2);
    }
};

} // namespace
QTEST_GUILESS_MAIN(WireTest)
#include "wire-test.moc"
