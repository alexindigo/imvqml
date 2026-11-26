// SPDX-License-Identifier: LGPL-3.0-only
#include "wire.h"

int main() {
    Imv::SeqPacket transport{Imv::Fd(Imv::ChildSocket)};
    for (;;) {
        Imv::Frame frame;
        const auto received = transport.receive(frame);
        if (received == Imv::Io::Closed)
            return 0;
        if (received != Imv::Io::Ok || frame.kind != Imv::Kind::Probe)
            return 1;
        frame.kind = Imv::Kind::ProbeReply;
        if (transport.send(frame) != Imv::Io::Ok)
            return 1;
    }
}
