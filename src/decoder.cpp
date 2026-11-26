// SPDX-License-Identifier: LGPL-3.0-only
#include "wire.h"

#include <sys/resource.h>

int main() {
    constexpr rlim_t addressBudget = 1024ull * 1024 * 1024;
    const rlimit address{addressBudget, addressBudget};
    const rlimit noCore{0, 0};
    if (::setrlimit(RLIMIT_AS, &address) != 0 || ::setrlimit(RLIMIT_CORE, &noCore) != 0)
        return 2;
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
        else if (frame.kind == Imv::Kind::Request)
            frame.kind = Imv::Kind::Reply;
        else
            return 1;
        if (transport.send(frame) != Imv::Io::Ok)
            return 1;
    }
}
