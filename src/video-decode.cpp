// SPDX-License-Identifier: LGPL-3.0-only
// Native transaction adapted from mpvqml v0.3.0, mpvframecapture.cpp.
#include "video-decode.h"
#include "private/video-signature.h"

#include <unistd.h>

#ifdef IMV_HAVE_MPV_HEADERS
#include <QImage>
#include <QTransform>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <memory>
#include <mpv/client.h>
#include <mpv/render.h>
#endif

namespace Imv {
namespace {
const char *videoDemuxer(int input) {
    char header[VideoSignatureBytes];
    const auto size = ::pread(input, header, sizeof header, 0);
    return size < 0 ? nullptr : Imv::videoDemuxer(QByteArrayView(header, size));
}

#ifdef IMV_HAVE_MPV_HEADERS
// Capture is one software frame, not playback. Do not let libavcodec size
// its decoder worker pool from the machine's CPU count.
constexpr char CaptureDecoderThreads[] = "1";

struct MpvApi {
    static void *open() {
#ifdef IMV_MPV_RUNTIME_HINT
        return ::dlopen(IMV_MPV_RUNTIME_HINT, RTLD_NOW | RTLD_LOCAL);
#else
        return ::dlopen("libmpv.so.2", RTLD_NOW | RTLD_LOCAL);
#endif
    }
    void *library = open();
#define IMV_MPV_SYMBOL(name) decltype(&::mpv_##name) name = symbol<decltype(name)>("mpv_" #name)
    IMV_MPV_SYMBOL(client_api_version);
    IMV_MPV_SYMBOL(create);
    IMV_MPV_SYMBOL(initialize);
    IMV_MPV_SYMBOL(terminate_destroy);
    IMV_MPV_SYMBOL(set_option_string);
    IMV_MPV_SYMBOL(command_async);
    IMV_MPV_SYMBOL(wait_event);
    IMV_MPV_SYMBOL(request_log_messages);
    IMV_MPV_SYMBOL(error_string);
    IMV_MPV_SYMBOL(get_property);
    IMV_MPV_SYMBOL(render_context_create);
    IMV_MPV_SYMBOL(render_context_free);
    IMV_MPV_SYMBOL(render_context_update);
    IMV_MPV_SYMBOL(render_context_render);
#undef IMV_MPV_SYMBOL

    ~MpvApi() {
        if (library)
            ::dlclose(library);
    }
    template <class Symbol> Symbol symbol(const char *name) const {
        return library ? reinterpret_cast<Symbol>(::dlsym(library, name)) : nullptr;
    }
    explicit operator bool() const {
        return client_api_version && client_api_version() >> 16 == 2 && create && initialize &&
               terminate_destroy && set_option_string && command_async && wait_event &&
               request_log_messages && error_string && get_property && render_context_create &&
               render_context_free && render_context_update && render_context_render;
    }
};

struct NativeResult {
    int value;
    int systemError;
};

template <class Call> NativeResult native(Call call) {
    errno = 0;
    const int value = call();
    return {value, errno};
}

QByteArray peakAddressSpace() {
    const int fd = ::open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return "unavailable";
    char status[4096];
    const auto size = ::read(fd, status, sizeof status);
    ::close(fd);
    if (size <= 0)
        return "unavailable";
    const QByteArrayView view(status, size);
    const auto start = view.indexOf("\nVmPeak:");
    const auto end = start < 0 ? -1 : view.indexOf('\n', start + 8);
    return end < 0 ? QByteArray("unavailable")
                   : view.sliced(start + 8, end - start - 8).trimmed().toByteArray();
}

struct VideoDiagnostics {
    const MpvApi &api;
    mpv_handle *core;
    // No heap storage or diagnostic output on success. Bound both the number
    // and length of entries independently of the input and libmpv's queue.
    std::array<std::array<char, 120>, 5> logs;
    size_t next = 0, count = 0;

    void record(const mpv_event &event) {
        if (event.event_id != MPV_EVENT_LOG_MESSAGE)
            return;
        const auto &line = *static_cast<const mpv_event_log_message *>(event.data);
        auto &entry = logs[next];
        auto size = qBound(0,
                           std::snprintf(entry.data(), entry.size(), "%.20s/%.8s: %.119s",
                                         line.prefix, line.level, line.text),
                           int(entry.size() - 1));
        while (size && (entry[size - 1] == '\n' || entry[size - 1] == '\r'))
            entry[--size] = '\0';
        for (int i = 0; i < size; ++i)
            if (const auto c = static_cast<unsigned char>(entry[i]); c < 32 || c > 126)
                entry[i] = '?';
        next = (next + 1) % logs.size();
        count = qMin(count + 1, logs.size());
    }

    DecodeReport fail(DecodeCode code, const char *summary, const char *call,
                      std::optional<int> mpvError = {}, int systemError = 0) {
        // Pull warnings still queued when the failing event/call arrived.
        if (core)
            for (int i = 0; i < 64; ++i) {
                const auto &event = *api.wait_event(core, 0);
                if (event.event_id == MPV_EVENT_NONE)
                    break;
                record(event);
            }
        QString detail = QString::fromLatin1(summary);
        detail += QStringLiteral("; native=%1").arg(QString::fromLatin1(call));
        if (mpvError)
            detail += QStringLiteral("; mpv=%1 (%2)")
                          .arg(*mpvError)
                          .arg(QString::fromUtf8(api.error_string(*mpvError)).left(80));
        detail +=
            QStringLiteral("; errno=%1; VmPeak=%2")
                .arg(systemError ? QString::number(systemError) : QStringLiteral("unavailable"),
                     QString::fromLatin1(peakAddressSpace()));
        for (size_t i = 0; i < count; ++i)
            detail +=
                QStringLiteral("\n") +
                QString::fromLatin1(logs[(next + logs.size() - count + i) % logs.size()].data());
        return {code, {}, {0, 0}, std::move(detail)};
    }
};

// Replies and untagged media events are two independent readiness gates.
// An earlier restart cannot make a later seek ready.
struct Transition {
    uint64_t token = 1;
    bool acknowledged = false;
    bool began = false;
    bool restarted = false;

    bool accept(const mpv_event &event) {
        switch (event.event_id) {
        case MPV_EVENT_COMMAND_REPLY:
            if (event.reply_userdata == token && !acknowledged) {
                if (event.error < 0)
                    return false;
                acknowledged = true;
            }
            break;
        case MPV_EVENT_FILE_LOADED:
        case MPV_EVENT_SEEK:
            if (event.event_id == (token == 1 ? MPV_EVENT_FILE_LOADED : MPV_EVENT_SEEK)) {
                began = true;
                restarted = false;
            }
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            restarted = began;
            break;
        case MPV_EVENT_END_FILE:
        case MPV_EVENT_SHUTDOWN:
        case MPV_EVENT_QUEUE_OVERFLOW:
            return false;
        default:
            break;
        }
        return true;
    }
    bool ready() const { return acknowledged && restarted; }
};

std::optional<DecodeReport> load(VideoDiagnostics &diagnostics, mpv_render_context *render,
                                 int input, double fraction) {
    const auto &api = diagnostics.api;
    auto *mpv = diagnostics.core;
    const auto fail = [&](const char *call, std::optional<int> mpvError, int systemError = 0) {
        return diagnostics.fail(DecodeCode::Decode, "Could not load or seek to a video frame", call,
                                mpvError, systemError);
    };
    const auto fdName = "fd://" + QByteArray::number(input);
    const char *command[]{"loadfile", fdName.constData(), nullptr};
    if (const auto rc = native([&] { return api.command_async(mpv, 1, command); }); rc.value < 0)
        return fail("mpv_command_async(loadfile)", rc.value, rc.systemError);
    Transition events;
    // The process pool enforces the independent hard deadline, including
    // native initialization and destruction which cannot be interrupted here.
    for (;;) {
        const auto &event = *api.wait_event(mpv, 0.01);
        diagnostics.record(event);
        if (!events.accept(event)) {
            switch (event.event_id) {
            case MPV_EVENT_COMMAND_REPLY:
                return fail(events.token == 1 ? "MPV_EVENT_COMMAND_REPLY(loadfile)"
                                              : "MPV_EVENT_COMMAND_REPLY(seek)",
                            event.error);
            case MPV_EVENT_END_FILE: {
                const auto &end = *static_cast<const mpv_event_end_file *>(event.data);
                return fail(events.token == 1 ? "MPV_EVENT_END_FILE(loadfile)"
                                              : "MPV_EVENT_END_FILE(seek)",
                            end.reason == MPV_END_FILE_REASON_ERROR ? std::optional(end.error)
                                                                    : std::nullopt);
            }
            case MPV_EVENT_QUEUE_OVERFLOW:
                return fail("MPV_EVENT_QUEUE_OVERFLOW", {});
            default:
                return fail("MPV_EVENT_SHUTDOWN", {});
            }
        }
        if (events.ready()) {
            double duration = 0;
            if (!(events.token == 1 && fraction > 0 &&
                  api.get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) >= 0 &&
                  std::isfinite(duration) && duration > 0))
                return {};
            const auto time = QByteArray::number(duration * qMin(fraction, 0.999), 'g', 17);
            const char *seek[]{"seek", time.constData(), "absolute+exact", nullptr};
            if (const auto rc = native([&] { return api.command_async(mpv, 2, seek); });
                rc.value < 0)
                return fail("mpv_command_async(seek)", rc.value, rc.systemError);
            events = {2};
        }
        if (api.render_context_update(render) & MPV_RENDER_UPDATE_FRAME) {
            int skip = 1, block = 0;
            mpv_render_param options[]{{MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
                                       {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                                       {}};
            if (const auto rc = native([&] { return api.render_context_render(render, options); });
                rc.value < 0)
                return fail("mpv_render_context_render(skip)", rc.value, rc.systemError);
        }
    }
}

QImage renderFrame(const MpvApi &api, mpv_render_context *render, QSize size, int rotation,
                   NativeResult &result, const char *&call) {
    errno = 0;
    QImage image(size, QImage::Format_RGBX8888);
    if (image.isNull()) {
        result = {0, errno};
        call = "QImage(allocation)";
        return {};
    }
    int dimensions[]{size.width(), size.height()};
    size_t stride = image.bytesPerLine();
    int block = 0;
    char format[] = "rgb0";
    mpv_render_param params[]{{MPV_RENDER_PARAM_SW_SIZE, dimensions},
                              {MPV_RENDER_PARAM_SW_FORMAT, format},
                              {MPV_RENDER_PARAM_SW_STRIDE, &stride},
                              {MPV_RENDER_PARAM_SW_POINTER, image.bits()},
                              {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                              {}};
    result = native([&] { return api.render_context_render(render, params); });
    if (result.value < 0) {
        call = "mpv_render_context_render(frame)";
        return {};
    }
    for (int y = 0; y < image.height(); ++y) {
        auto *row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x)
            row[x * 4 + 3] = 255;
    }
    errno = 0;
    if (rotation)
        image = image.transformed(QTransform().rotate(rotation));
    result = {0, errno};
    call = "QImage(transform/format)";
    return image.reinterpretAsFormat(QImage::Format_RGBA8888) ? image : QImage{};
}
#endif
} // namespace

std::optional<DecodeReport> decodeVideo(int input, int output, const DecodeSpec &spec) {
    const auto *demuxer = videoDemuxer(input);
    if (!demuxer || spec.mode == DecodeSpec::FrameAt)
        return {};
    const auto fail = [](DecodeCode code, const char *detail) {
        return DecodeReport{code, {}, {0, 0}, QString::fromLatin1(detail)};
    };
#ifndef IMV_HAVE_MPV_HEADERS
    return fail(DecodeCode::Unsupported, "video-support-unavailable");
#else
    // ABI and symbol checks are only the first gate. An ABI-2 runtime
    // must also accept the restricted configuration and create the SW renderer.
    const MpvApi api;
    if (!api)
        return fail(DecodeCode::Unsupported, "video-support-unavailable");
    errno = 0;
    const std::unique_ptr<mpv_handle, decltype(api.terminate_destroy)> core(api.create(),
                                                                            api.terminate_destroy);
    const int createError = errno;
    VideoDiagnostics diagnostics{api, core.get()};
    const auto unavailable = [&](const char *call, std::optional<int> error, int systemError) {
        return diagnostics.fail(DecodeCode::Unsupported, "video-support-unavailable", call, error,
                                systemError);
    };
    if (!core)
        return unavailable("mpv_create", {}, createError);
    // Optional observation must not make an otherwise usable runtime fail.
    api.request_log_messages(core.get(), "warn");
    struct Option {
        const char *name;
        const char *value;
        bool optional = false;
    };
    const Option options[]{{"config", "no"},
                           {"terminal", "no"},
                           {"load-scripts", "no"},
                           {"access-references", "no"},
                           {"vo", "libmpv"},
                           {"vf", "format=rotate=0"},
                           {"aid", "no"},
                           {"sid", "no"},
                           {"audio-file-auto", "no"},
                           {"sub-auto", "no"},
                           {"pause", "yes"},
                           {"keep-open", "yes"},
                           {"hwdec", "no"},
                           {"vd-lavc-threads", CaptureDecoderThreads},
                           {"vd-lavc-o", "max_pixels=67108864"},
                           {"cache", "no"},
                           {"osd-level", "0"},
                           {"demuxer", "lavf"},
                           {"demuxer-lavf-format", demuxer},
                           {"demuxer-lavf-o", "protocol_whitelist=fd"},
                           {"osc", "no", true},
                           {"ytdl", "no", true},
                           {"load-stats-overlay", "no", true},
                           {"load-console", "no", true},
                           {"load-osd-console", "no", true},
                           {"load-auto-profiles", "no", true},
                           {"load-select", "no", true},
                           {"load-positioning", "no", true},
                           {"load-commands", "no", true},
                           {"load-context-menu", "no", true}};
    for (const auto &[name, value, optional] : options) {
        const auto rc = native([&] { return api.set_option_string(core.get(), name, value); });
        if (rc.value < 0 && !(optional && rc.value == MPV_ERROR_OPTION_NOT_FOUND)) {
            const auto call = QByteArray("mpv_set_option_string(") + name + ')';
            return unavailable(call.constData(), rc.value, rc.systemError);
        }
    }
    if (const auto rc = native([&] { return api.initialize(core.get()); }); rc.value < 0)
        return unavailable("mpv_initialize", rc.value, rc.systemError);
    char renderer[] = MPV_RENDER_API_TYPE_SW;
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, renderer}, {}};
    mpv_render_context *raw = nullptr;
    if (const auto rc = native([&] { return api.render_context_create(&raw, core.get(), params); });
        rc.value < 0 || !raw)
        return unavailable("mpv_render_context_create", rc.value, rc.systemError);
    const std::unique_ptr<mpv_render_context, decltype(api.render_context_free)> render(
        raw, api.render_context_free);
    if (auto failure = load(diagnostics, render.get(), input, spec.position))
        return std::move(*failure);

    // Strip rotation before the SW renderer (vf format), but read it from the
    // original video params and apply it to the owned pixels afterwards.
    int64_t rotation = 0, width = 0, height = 0;
    if (api.get_property(core.get(), "video-params/rotate", MPV_FORMAT_INT64, &rotation) < 0 ||
        rotation % 90)
        rotation = 0;
    rotation %= 360;
    const auto readDimension = [&](const char *property, int64_t &value) {
        return native(
            [&] { return api.get_property(core.get(), property, MPV_FORMAT_INT64, &value); });
    };
    for (auto [property, value] :
         {std::pair{"video-out-params/dw", &width}, std::pair{"video-out-params/dh", &height}}) {
        if (const auto rc = readDimension(property, *value); rc.value < 0)
            return diagnostics.fail(DecodeCode::PixelLimit, "Video dimensions are out of bounds",
                                    property, rc.value, rc.systemError);
    }
    if (width < 1 || height < 1 || width > INT_MAX || height > INT_MAX ||
        width * height > MaxSourcePixels)
        return diagnostics.fail(DecodeCode::PixelLimit, "Video dimensions are out of bounds",
                                "video-out-params geometry");
    const bool transpose = rotation % 180 != 0;
    const QSize oriented = transpose ? QSize(height, width) : QSize(width, height);
    DecodeReport report;
    report.source = {"video", oriented, 1, 1};
    if (spec.mode == DecodeSpec::Inspect)
        return report;
    auto target =
        oriented.scaled(spec.bounds, Qt::KeepAspectRatio).boundedTo(oriented).expandedTo({1, 1});
    if (spec.scale == DecodeSpec::Cover)
        target = oriented.scaled(spec.bounds, Qt::KeepAspectRatioByExpanding)
                     .boundedTo(oriented)
                     .boundedTo({MaxEdge, MaxEdge})
                     .expandedTo({1, 1});
    NativeResult renderResult{};
    const char *renderCall = nullptr;
    auto image = renderFrame(api, render.get(), transpose ? target.transposed() : target, rotation,
                             renderResult, renderCall);
    if (image.isNull())
        return diagnostics.fail(DecodeCode::Decode, "Could not render a video frame", renderCall,
                                renderResult.value < 0 ? std::optional(renderResult.value)
                                                       : std::nullopt,
                                renderResult.systemError);
    if (spec.scale == DecodeSpec::Cover) {
        const auto crop = spec.bounds.scaled(image.size(), Qt::KeepAspectRatio).expandedTo({1, 1});
        image = image.copy((image.width() - crop.width()) / 2, (image.height() - crop.height()) / 2,
                           crop.width(), crop.height());
    } else if (spec.scale == DecodeSpec::Stretch) {
        image = image.scaled(spec.bounds.boundedTo(oriented), Qt::IgnoreAspectRatio,
                             Qt::SmoothTransformation);
    }
    report.output = image.size();
    const auto bytes = rgbaBytes(report.output);
    if (!bytes)
        return diagnostics.fail(DecodeCode::Output, "Could not write video frame",
                                "QImage(output geometry)");
    errno = 0;
    if (!writeAll(output, reinterpret_cast<const char *>(image.constBits()), bytes))
        return diagnostics.fail(DecodeCode::Output, "Could not write video frame", "pwrite", {},
                                errno);
    return report;
#endif
}
} // namespace Imv
