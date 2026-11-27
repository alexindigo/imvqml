// SPDX-License-Identifier: LGPL-3.0-only
#pragma once

#include "decode-wire.h"
#include "pool.h"
#include <QImage>
#include <QUrl>
#include <QVariantMap>
#include <optional>
#include <sys/stat.h>
#include <variant>

namespace Imv {
struct OpenedSource {
    Fd fd;
    QByteArray identity;
    Failure error;
    std::optional<struct stat> file;
};
struct AnimationFrames {
    std::shared_ptr<const uchar> mapping;
    QSize size;
    int count = 0;
    QImage frame(int index) const;
    int delay(int index) const;
};
struct Decoded {
    SourceInfo source;
    QImage pixels;
    Failure error;
    AnimationFrames animation;
    bool stream = false;
    int delay = 0;
};

QString localPath(const QUrl &url);
enum class SourceLimit { Decode, Strict };
struct CapturedSource {
    QUrl data;
    Fd fd;
    struct stat file{};
    Failure error;
    SourceLimit limit = SourceLimit::Decode;
};
CapturedSource captureSource(const QUrl &url, SourceLimit limit = SourceLimit::Decode);
OpenedSource openSource(const QUrl &url);
OpenedSource openSource(const QByteArray &bytes);
using SourceInput = std::variant<QUrl, QByteArray>;
using PreparationInput = std::variant<SourceInput, CapturedSource>;
using SourceReady = std::function<void(OpenedSource)>;
// Empty means admission was busy; destroying a live handle revokes delivery
// and releases a source that has not entered the worker yet.
std::shared_ptr<void> prepareSource(PreparationInput &input, SourceReady ready);
void onSourceCapacity(QObject *receiver, std::function<void()> released);
Pool &decoderPool();
Fd duplicateSource(const Fd &fd);
Pool::Ticket submitDecode(Pool &pool, Fd input, DecodeSpec spec, std::function<void(Decoded)> done);
QVariantMap asMap(const SourceInfo &source);
QVariantMap asMap(const Failure &failure);
} // namespace Imv
