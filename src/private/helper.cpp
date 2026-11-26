// SPDX-License-Identifier: LGPL-3.0-only
#include "helper.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

namespace Imv {

QString decoderPath() {
    Dl_info info{};
    // Resolve from this library, not the application path, cwd, or $PATH.
    static const char anchor = 0;
    if (!::dladdr(&anchor, &info))
        return {};
    const auto library = QFileInfo(QFile::decodeName(info.dli_fname)).canonicalFilePath();
    if (library.isEmpty())
        return {};
    return QDir::cleanPath(QFileInfo(library).absolutePath() + '/' + IMV_DECODER_FROM_LIB);
}

Fd startChild(QProcess &process, const QString &executable, const QStringList &args) {
    if (!QFileInfo(executable).isAbsolute() || process.state() != QProcess::NotRunning)
        return {};
    auto [local, remote] = socketPair();
    if (!local)
        return {};
    // The child endpoint can initially be fd 3. Always duplicate from a
    // higher fd: dup2 then reliably clears CLOEXEC on the inherited fd.
    Fd inherited(::fcntl(remote.get(), F_DUPFD_CLOEXEC, ChildSocket + 1));
    const int flags = ::fcntl(local.get(), F_GETFL);
    if (!inherited || flags < 0 || ::fcntl(local.get(), F_SETFL, flags | O_NONBLOCK) < 0)
        return {};
    process.setChildProcessModifier([fd = inherited.get(), &process] {
        if (::dup2(fd, ChildSocket) < 0)
            process.failChildProcessModifier("dup2 decoder socket", errno);
    });
    QProcess::UnixProcessParameters params;
    params.flags = QProcess::UnixProcessFlag::CloseFileDescriptors;
    params.lowestFileDescriptorToClose = ChildSocket + 1;
    process.setUnixProcessParameters(params);
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(executable, args, QIODevice::NotOpen);
    process.setChildProcessModifier({});
    return local;
}

} // namespace Imv
