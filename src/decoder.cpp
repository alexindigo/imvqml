// SPDX-License-Identifier: LGPL-3.0-only
#include "video-decode.h"
#include "vips-decode.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>

namespace {
Imv::DecodeReport decodeInput(int input, int output, const Imv::DecodeSpec &spec) {
    using namespace Imv;
    struct stat stat{};
    const int seals = ::fcntl(input, F_GET_SEALS);
    if (seals < 0 || (seals & MemfdSeals) != MemfdSeals || ::fstat(input, &stat) ||
        !S_ISREG(stat.st_mode))
        return {DecodeCode::Unreadable, {}, {0, 0}, "Input must be a sealed regular descriptor"};
    if (stat.st_size > MaxSourceBytes)
        return {DecodeCode::SourceLimit, {}, {0, 0}, "Source exceeds 256 MiB"};
    auto result = decodeWithVips(input, output, spec, stat.st_size);
    if (result.code == DecodeCode::Unsupported)
        if (auto video = decodeVideo(input, output, spec))
            return std::move(*video);
    return result;
}
} // namespace

int main(int, char **argv) {
    constexpr rlim_t addressBudget = 1024ull * 1024 * 1024;
    const rlimit address{addressBudget, addressBudget};
    const rlimit noCore{0, 0};
    if (::setrlimit(RLIMIT_AS, &address) != 0 || ::setrlimit(RLIMIT_CORE, &noCore) != 0)
        return 2;
    if (!Imv::initializeVips(argv[0]))
        return 3;
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
    }
}
