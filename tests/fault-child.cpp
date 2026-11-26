// SPDX-License-Identifier: LGPL-3.0-only
#include "wire.h"

#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    rlimit noCore{0, 0};
    if (::setrlimit(RLIMIT_CORE, &noCore) != 0)
        return 2;
    Imv::SeqPacket peer{Imv::Fd(Imv::ChildSocket)};
    for (Imv::Frame request; peer.receive(request) == Imv::Io::Ok;) {
        if (request.kind != Imv::Kind::Request)
            return 3;
        if (request.body == "hold") {
            pid_t pid = ::getpid();
            if (request.fds.size() != 1 ||
                ::write(request.fds[0].get(), &pid, sizeof pid) != sizeof pid)
                return 4;
            for (;;)
                ::pause();
        }
        if (request.body == "garbage") {
            if (::send(peer.descriptor(), "bad", 3, MSG_NOSIGNAL) != 3)
                return 5;
            for (;;)
                ::pause();
        }
        Imv::Frame answer{Imv::Kind::Reply, request.serial, QByteArray::number(::getpid()), {}};
        if (request.body == "wrong-serial")
            ++answer.serial;
        if (peer.send(answer) != Imv::Io::Ok)
            return 6;
    }
    return 0;
}
