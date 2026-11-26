// SPDX-License-Identifier: LGPL-3.0-only
#include "vips-decode.h"
#include <QtEndian>

#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vips/vips.h>

namespace Imv {
namespace {

struct Loader {
    const char *type;
    const char *operation;
    const char *format;
};

// This is both the admission list and the only place enabling libvips loaders.
constexpr Loader Loaders[] = {
    {"VipsForeignLoadPngSource", "pngload_source", "png"},
    {"VipsForeignLoadJpegSource", "jpegload_source", "jpeg"},
    {"VipsForeignLoadNsgifSource", "gifload_source", "gif"},
    {"VipsForeignLoadWebpSource", "webpload_source", "webp"},
    {"VipsForeignLoadTiffSource", "tiffload_source", "tiff"},
    {"VipsForeignLoadSvgSource", "svgload_source", "svg"},
    {"VipsForeignLoadHeifSource", "heifload_source", "heif"},
    {"VipsForeignLoadJxlSource", "jxlload_source", "jxl"},
};

struct Unref {
    void operator()(void *ptr) const { g_object_unref(ptr); }
};
using Image = std::unique_ptr<VipsImage, Unref>;
using Source = std::unique_ptr<VipsSource, Unref>;

struct ReadCap {
    int fd;
    qint64 limit;
    qint64 position = 0;

    static gint64 read(VipsSourceCustom *, void *buffer, gint64 count, ReadCap *cap) {
        if (count < 0 || cap->position < 0 || cap->position > cap->limit)
            return -1;
        ssize_t total;
        do {
            total = ::pread(cap->fd, buffer, qMin<qint64>(count, cap->limit - cap->position),
                            cap->position);
        } while (total < 0 && errno == EINTR);
        if (total > 0)
            cap->position += total;
        return total;
    }

    static gint64 seek(VipsSourceCustom *, gint64 delta, int origin, ReadCap *cap) {
        const auto base = origin == SEEK_SET ? 0 : origin == SEEK_END ? cap->limit : cap->position;
        if ((origin != SEEK_SET && origin != SEEK_END && origin != SEEK_CUR) || delta < -base ||
            delta > cap->limit - base)
            return -1;
        return cap->position = base + delta;
    }
};

DecodeReport fail(DecodeCode code, QString message, bool fromVips = false) {
    if (fromVips) {
        const auto detail = QString::fromUtf8(vips_error_buffer()).trimmed();
        if (!detail.isEmpty())
            message += QStringLiteral(": ") + detail;
    }
    vips_error_clear();
    return {code, {}, QSize(0, 0), std::move(message)};
}

bool avif(int input) {
    // The two ISO BMFF variants use the same libvips loader. Distinguish
    // brands only within the initial, bounded ftyp box.
    char bytes[128];
    const auto n = ::pread(input, bytes, sizeof bytes, 0);
    if (n < 12 || QByteArrayView(bytes + 4, 4) != "ftyp")
        return false;
    const auto *p = reinterpret_cast<const unsigned char *>(bytes);
    const quint32 len = quint32(p[0]) << 24 | quint32(p[1]) << 16 | quint32(p[2]) << 8 | p[3];
    for (int i = 8; i + 4 <= qMin<qint64>(n, len); i += 4) {
        if (i != 12 &&
            (QByteArrayView(bytes + i, 4) == "avif" || QByteArrayView(bytes + i, 4) == "avis"))
            return true;
    }
    return false;
}

} // namespace

bool initializeVips(const char *program) {
    if (VIPS_INIT(program))
        return false;
    vips_concurrency_set(2);
    vips_cache_set_max(0);
    vips_block_untrusted_set(TRUE);
    vips_operation_block_set("VipsForeignLoad", TRUE);
    for (const auto &loader : Loaders)
        vips_operation_block_set(loader.type, FALSE);
    return true;
}

DecodeReport decodeWithVips(int input, int output, const DecodeSpec &spec) {
    struct stat stat{};
    if (::fstat(input, &stat) != 0 || !S_ISREG(stat.st_mode))
        return fail(DecodeCode::Unreadable, "Input descriptor is not a readable regular file");
    if (stat.st_size > MaxSourceBytes)
        return fail(DecodeCode::SourceLimit, "Source exceeds 256 MiB");
    if (stat.st_size <= 0)
        return fail(DecodeCode::InvalidHeader, "Empty source");

    ReadCap cap{input, stat.st_size};
    auto *custom = vips_source_custom_new();
    Source source(VIPS_SOURCE(custom));
    g_signal_connect(custom, "read", G_CALLBACK(ReadCap::read), &cap);
    g_signal_connect(custom, "seek", G_CALLBACK(ReadCap::seek), &cap);
    const Loader *loader = nullptr;
    for (const auto &candidate : Loaders) {
        if (vips_foreign_is_a_source(candidate.operation, source.get())) {
            loader = &candidate;
            break;
        }
        vips_error_clear();
    }
    if (!loader)
        return fail(DecodeCode::Unsupported, "No allowed decoder recognizes this source");

    VipsImage *raw = nullptr;
    if (vips_source_rewind(source.get()) ||
        vips_call(loader->operation, source.get(), &raw, "access", VIPS_ACCESS_SEQUENTIAL,
                  "fail_on", VIPS_FAIL_ON_ERROR, nullptr))
        return fail(DecodeCode::InvalidHeader, "Could not inspect image header", true);
    Image image(raw);
    QSize size(vips_image_get_width(raw), vips_image_get_page_height(raw));
    if (size.width() < 1 || size.height() < 1)
        return fail(DecodeCode::InvalidHeader, "Nonpositive image dimensions");
    if (qint64(size.width()) * size.height() > MaxSourcePixels)
        return fail(DecodeCode::PixelLimit, "Source frame exceeds 64 MiPixels");
    const int orientation = vips_image_get_orientation(raw);
    if (orientation >= 5 && orientation <= 8)
        size.transpose();
    const int pages = qMax(1, vips_image_get_n_pages(raw));
    int loop = 1;
    if (pages > 1 && vips_image_get_typeof(raw, "loop")) {
        int count = 0;
        if (!vips_image_get_int(raw, "loop", &count))
            loop = qMax(0, count);
    }
    DecodeReport report;
    report.source = {QString::fromLatin1(QByteArrayView(loader->format) == "heif" && avif(input)
                                             ? "avif"
                                             : loader->format),
                     size, pages, loop};
    int *delays = nullptr;
    int delayCount = 0;
    if (spec.mode != DecodeSpec::Pixels && pages > 1 && vips_image_get_typeof(raw, "delay") &&
        !vips_image_get_array_int(raw, "delay", &delays, &delayCount) && delayCount == pages) {
        if (spec.page >= pages)
            return fail(DecodeCode::Decode, "Animation frame index is out of range");
        report.delay = qMax(1, delays[spec.page]);
    } else if (spec.mode == DecodeSpec::FrameAt) {
        return fail(DecodeCode::Decode, "Source no longer has animation timing");
    }
    if (spec.mode == DecodeSpec::Inspect)
        return report;
    auto target = spec.bounds;
    if (spec.scale == DecodeSpec::Cover) {
        target = target.scaled(size.boundedTo(target), Qt::KeepAspectRatio).expandedTo({1, 1});
    } else if (spec.scale == DecodeSpec::Stretch) {
        target = target.boundedTo(size);
    }
    const auto thumbnail = [&](int page) -> Image {
        // Always request one composited page; never construct a full-height stack.
        const auto options = page ? "page=" + QByteArray::number(page) + ",n=1" : QByteArray{};
        if (vips_source_rewind(source.get()) ||
            vips_thumbnail_source(
                source.get(), &raw, target.width(), "height", target.height(), "crop",
                spec.scale == DecodeSpec::Cover ? VIPS_INTERESTING_CENTRE : VIPS_INTERESTING_NONE,
                "size", spec.scale == DecodeSpec::Stretch ? VIPS_SIZE_FORCE : VIPS_SIZE_DOWN,
                "fail_on", VIPS_FAIL_ON_ERROR, "option-string", options.constData(), nullptr))
            return {};
        Image frame(raw);
        if (vips_colourspace(frame.get(), &raw, VIPS_INTERPRETATION_sRGB, nullptr))
            return {};
        frame.reset(raw);
        if (!vips_image_hasalpha(raw)) {
            if (vips_addalpha(frame.get(), &raw, nullptr))
                return {};
            frame.reset(raw);
        }
        if (vips_image_get_bands(raw) != 4 || vips_image_get_format(raw) != VIPS_FORMAT_UCHAR)
            return {};
        return frame;
    };
    auto frame = thumbnail(spec.page);
    if (!frame)
        return fail(DecodeCode::Decode, "Could not thumbnail RGBA image", true);
    const auto dimensions = [](const Image &value) {
        return QSize(vips_image_get_width(value.get()), vips_image_get_height(value.get()));
    };
    report.output = dimensions(frame);
    const auto bytes = rgbaBytes(report.output);
    if (bytes <= 0 || report.output.width() > spec.bounds.width() ||
        report.output.height() > spec.bounds.height())
        return fail(DecodeCode::Output, "Output geometry is out of bounds");
    if (spec.mode == DecodeSpec::Animation && report.delay) {
        report.layout = DecodeReport::Batch;
        if (report.outputBytes())
            report.delay = 0;
        else
            report.layout = DecodeReport::Stream;
    }
    const int count = report.layout == DecodeReport::Batch ? pages : 1;
    for (int i = 0; i < count; ++i) {
        if (i)
            frame = thumbnail(i);
        if (!frame || dimensions(frame) != report.output)
            return fail(DecodeCode::Decode, "Inconsistent animation frame geometry", true);
        size_t decoded = 0;
        std::unique_ptr<void, decltype(&g_free)> pixels(
            vips_image_write_to_memory(frame.get(), &decoded), g_free);
        frame.reset();
        if (!pixels || decoded != size_t(bytes))
            return fail(DecodeCode::Decode, "Could not materialize RGBA pixels", true);
        qint64 offset = 0;
        if (count > 1) {
            offset = i * (bytes + 4);
            const auto delay = qToLittleEndian(qint32(qMax(1, delays[i])));
            if (!writeAll(output, reinterpret_cast<const char *>(&delay), 4, offset))
                return fail(DecodeCode::Output, "Could not write animation timing");
            offset += 4;
        }
        if (!writeAll(output, static_cast<const char *>(pixels.get()), bytes, offset))
            return fail(DecodeCode::Output, "Could not write RGBA pixels");
    }
    return report;
}

} // namespace Imv
