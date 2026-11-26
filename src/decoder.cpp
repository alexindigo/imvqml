// SPDX-License-Identifier: LGPL-3.0-only
#include "video-decode.h"
#include "vips-decode.h"
#include <QCoreApplication>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>

namespace {
Imv::DecodeReport decodeInput(int input, int output, const Imv::DecodeSpec &spec) {
    using namespace Imv;
    struct stat stat{};
    if (::fstat(input, &stat) || !S_ISREG(stat.st_mode))
        return {DecodeCode::Unreadable, {}, {0, 0}, "Input must be a regular descriptor"};
    const int seals = ::fcntl(input, F_GET_SEALS);
    const bool sealed = seals >= 0 && (seals & MemfdSeals) == MemfdSeals;
    if (stat.st_size > MaxSourceBytes) {
        const int flags = ::fcntl(input, F_GETFL);
        if (!sealed && (flags < 0 || (flags & (O_ACCMODE | O_PATH)) != O_RDONLY))
            return {DecodeCode::Unreadable, {}, {0, 0}, "Stream must be a read-only descriptor"};
        if (auto video = decodeVideo(input, output, spec)) {
            struct stat after{};
            if (!sealed && (::fstat(input, &after) || after.st_size != stat.st_size ||
                            after.st_mtim.tv_sec != stat.st_mtim.tv_sec ||
                            after.st_mtim.tv_nsec != stat.st_mtim.tv_nsec))
                return {DecodeCode::Unreadable, {}, {0, 0}, "Stream changed during frame capture"};
            return std::move(*video);
        }
        return {DecodeCode::SourceLimit, {}, {0, 0}, "Source exceeds 256 MiB"};
    }
    if (!sealed)
        return {DecodeCode::Unreadable, {}, {0, 0}, "Image sources must be sealed"};
    auto result = decodeImage(input, output, spec, stat.st_size);
    if (result.code == DecodeCode::Unsupported)
        if (auto video = decodeVideo(input, output, spec))
            return std::move(*video);
    return result;
}
} // namespace

int main(int argc, char **argv) {
    constexpr rlim_t addressBudget = 1024ull * 1024 * 1024;
    const rlimit address{addressBudget, addressBudget};
    const rlimit noCore{0, 0};
    if (::setrlimit(RLIMIT_AS, &address) != 0 || ::setrlimit(RLIMIT_CORE, &noCore) != 0)
        return 2;
    const QCoreApplication application(argc, argv);
    if (!Imv::initializeVips(argv[0]))
        return 3;
    // Deadline enforcement must not depend on the Qt GUI event loop.
    sigevent expiry{};
    expiry.sigev_notify = SIGEV_SIGNAL;
    expiry.sigev_signo = SIGKILL;
    timer_t watchdog{};
    if (::timer_create(CLOCK_MONOTONIC, &expiry, &watchdog))
        return 4;
    Imv::SeqPacket transport{Imv::Fd(Imv::ChildSocket)};
    for (;;) {
        Imv::Frame frame;
        const auto received = transport.receive(frame);
        if (received == Imv::Io::Closed)
            return 0;
        if (received != Imv::Io::Ok)
            return 1;
        if (frame.kind == Imv::Kind::Probe)
            frame.kind = Imv::Kind::ProbeReply;
        else if (frame.kind == Imv::Kind::Request) {
            Imv::DecodeSpec spec;
            if (!Imv::decode(frame.body, spec) ||
                frame.fds.size() != (spec.mode == Imv::DecodeSpec::Inspect ? 1u : 2u))
                return 1;
            const auto input = frame.fds.front().get();
            const auto output = frame.fds.size() == 2 ? frame.fds.back().get() : -1;
            const auto ms = spec.mode >= Imv::DecodeSpec::Animation ? Imv::AnimationDeadlineMs
                                                                    : Imv::StillDeadlineMs;
            const itimerspec deadline{{}, {ms / 1000, (ms % 1000) * 1000000L}};
            if (::timer_settime(watchdog, 0, &deadline, nullptr))
                return 4;
            auto result = decodeInput(input, output, spec);
            if (output >= 0 && result.code == Imv::DecodeCode::Ok &&
                !Imv::sealPixelOutput(output, result.outputBytes())) {
                result = {Imv::DecodeCode::Output, {}, {0, 0}, "Could not seal the output memfd"};
            }
            frame.fds.clear();
            frame.body = Imv::encode(result);
            frame.kind = Imv::Kind::Reply;
        } else
            return 1;
        if (transport.send(frame) != Imv::Io::Ok)
            return 1;
        const itimerspec idle{};
        if (::timer_settime(watchdog, 0, &idle, nullptr))
            return 4;
    }
}
