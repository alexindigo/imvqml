// SPDX-License-Identifier: LGPL-3.0-only
// Native transaction adapted from mpvqml v0.3.0, mpvframecapture.cpp.
#include "video-decode.h"

#include <unistd.h>

#ifdef IMV_HAVE_MPV_HEADERS
#include <QImage>
#include <QTransform>
#include <cmath>
#include <dlfcn.h>
#include <fcntl.h>
#include <memory>
#include <mpv/client.h>
#include <mpv/render.h>
#endif

namespace Imv {
namespace {
const char *videoDemuxer(int input) {
    char header[16];
    const auto size = ::pread(input, header, sizeof header, 0);
    if (size < 12)
        return nullptr;
    const QByteArrayView signature(header, size);
    if (signature.startsWith(QByteArrayView("\x1a\x45\xdf\xa3", 4)))
        return "matroska";
    const auto box = signature.sliced(4, 4);
    if (box == "ftyp" || box == "moov" || box == "mdat" || box == "wide")
        return "mov";
    if (signature.startsWith("RIFF") && signature.sliced(8, 4) == "AVI ")
        return "avi";
    if (signature.startsWith("OggS"))
        return "ogg";
    return nullptr;
}

#ifdef IMV_HAVE_MPV_HEADERS
struct MpvApi {
    static void *open() {
        auto *library = ::dlopen("libmpv.so.2", RTLD_NOW | RTLD_LOCAL);
#ifdef IMV_MPV_RUNTIME_HINT
        if (!library)
            library = ::dlopen(IMV_MPV_RUNTIME_HINT, RTLD_NOW | RTLD_LOCAL);
#endif
        return library;
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
               get_property && render_context_create && render_context_free &&
               render_context_update && render_context_render;
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

bool load(const MpvApi &api, mpv_handle *mpv, mpv_render_context *render, int input,
          double fraction) {
    const auto fdName = "fd://" + QByteArray::number(input);
    const char *command[]{"loadfile", fdName.constData(), nullptr};
    if (api.command_async(mpv, 1, command) < 0)
        return false;
    Transition events;
    // The process pool enforces the independent hard deadline, including
    // native initialization and destruction which cannot be interrupted here.
    for (;;) {
        if (!events.accept(*api.wait_event(mpv, 0.01)))
            return false;
        if (events.ready()) {
            double duration = 0;
            if (!(events.token == 1 && fraction > 0 &&
                  api.get_property(mpv, "duration", MPV_FORMAT_DOUBLE, &duration) >= 0 &&
                  std::isfinite(duration) && duration > 0))
                return true;
            const auto time = QByteArray::number(duration * qMin(fraction, 0.999), 'g', 17);
            const char *seek[]{"seek", time.constData(), "absolute+exact", nullptr};
            if (api.command_async(mpv, 2, seek) < 0)
                return false;
            events = {2};
        }
        if (api.render_context_update(render) & MPV_RENDER_UPDATE_FRAME) {
            int skip = 1, block = 0;
            mpv_render_param options[]{{MPV_RENDER_PARAM_SKIP_RENDERING, &skip},
                                       {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
                                       {}};
            if (api.render_context_render(render, options) < 0)
                return false;
        }
    }
}

QImage renderFrame(const MpvApi &api, mpv_render_context *render, QSize size, int rotation) {
    QImage image(size, QImage::Format_RGBX8888);
    if (image.isNull())
        return {};
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
    if (api.render_context_render(render, params) < 0)
        return {};
    for (int y = 0; y < image.height(); ++y) {
        auto *row = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x)
            row[x * 4 + 3] = 255;
    }
    if (rotation)
        image = image.transformed(QTransform().rotate(rotation));
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
    const MpvApi api;
    if (!api)
        return fail(DecodeCode::Unsupported, "video-support-unavailable");
    const std::unique_ptr<mpv_handle, decltype(api.terminate_destroy)> core(api.create(),
                                                                            api.terminate_destroy);
    if (!core)
        return fail(DecodeCode::Decode, "Could not create the video decoder");
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
                           {"vd-lavc-threads", "1"},
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
        const int rc = api.set_option_string(core.get(), name, value);
        if (rc < 0 && !(optional && rc == MPV_ERROR_OPTION_NOT_FOUND))
            return fail(DecodeCode::Decode, "Could not configure the restricted video decoder");
    }
    if (api.initialize(core.get()) < 0)
        return fail(DecodeCode::Decode, "Could not initialize the video decoder");
    char renderer[] = MPV_RENDER_API_TYPE_SW;
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, renderer}, {}};
    mpv_render_context *raw = nullptr;
    if (api.render_context_create(&raw, core.get(), params) < 0)
        return fail(DecodeCode::Decode, "Could not initialize the software video renderer");
    const std::unique_ptr<mpv_render_context, decltype(api.render_context_free)> render(
        raw, api.render_context_free);
    if (!load(api, core.get(), render.get(), input, spec.position))
        return fail(DecodeCode::Decode, "Could not load or seek to a video frame");

    // Strip rotation before the SW renderer (vf format), but read it from the
    // original video params and apply it to the owned pixels afterwards.
    int64_t rotation = 0, width = 0, height = 0;
    if (api.get_property(core.get(), "video-params/rotate", MPV_FORMAT_INT64, &rotation) < 0 ||
        rotation % 90)
        rotation = 0;
    rotation %= 360;
    if (api.get_property(core.get(), "video-out-params/dw", MPV_FORMAT_INT64, &width) < 0 ||
        api.get_property(core.get(), "video-out-params/dh", MPV_FORMAT_INT64, &height) < 0 ||
        width < 1 || height < 1 || width > INT_MAX || height > INT_MAX ||
        width * height > MaxSourcePixels)
        return fail(DecodeCode::PixelLimit, "Video dimensions are out of bounds");
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
    auto image = renderFrame(api, render.get(), transpose ? target.transposed() : target, rotation);
    if (image.isNull())
        return fail(DecodeCode::Decode, "Could not render a video frame");
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
    if (!bytes || ::ftruncate(output, bytes) ||
        !writeAll(output, reinterpret_cast<const char *>(image.constBits()), bytes) ||
        ::fcntl(output, F_ADD_SEALS, MemfdSeals) < 0)
        return fail(DecodeCode::Output, "Could not publish the sealed video frame");
    return report;
#endif
}
} // namespace Imv
