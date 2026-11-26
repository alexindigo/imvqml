// SPDX-License-Identifier: LGPL-3.0-only
#include "video-decode.h"
#include "vips-decode.h"

#include <sys/resource.h>

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
            auto result = Imv::decodeWithVips(input, output, spec);
            if (result.code == Imv::DecodeCode::Unsupported)
                if (auto video = Imv::decodeVideo(input, output, spec))
                    result = std::move(*video);
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
