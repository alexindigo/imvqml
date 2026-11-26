// SPDX-License-Identifier: LGPL-3.0-only
#include "vips-decode.h"
#include <QColorSpace>
#include <QFile>
#include <QImageIOHandler>
#include <QImageReader>
#include <QtEndian>

#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
#include <vips/vips.h>

namespace Imv {
namespace {

enum class Input { Source, DescriptorPath, Qt };

struct Loader {
    const char *type;
    const char *operation;
    const char *format;
    Input input = Input::Source;
    QByteArrayView signature{};
};

// The only decoder allow-list. libvips' default block on untrusted operations
// stays on; these exact foreign operations are intentional helper-only
// exceptions (in particular PNM, JPEG 2000, HDR and EXR). Qt readers receive
// an explicit key; they do not probe arbitrary plugins on untrusted input.
constexpr Loader Loaders[] = {
    {"VipsForeignLoadPngSource", "pngload_source", "png"},
    {"VipsForeignLoadJpegSource", "jpegload_source", "jpeg"},
    {"VipsForeignLoadNsgifSource", "gifload_source", "gif"},
    {"VipsForeignLoadWebpSource", "webpload_source", "webp"},
    {"VipsForeignLoadTiffSource", "tiffload_source", "tiff"},
    {"VipsForeignLoadSvgSource", "svgload_source", "svg"},
    {"VipsForeignLoadHeifSource", "heifload_source", "heif"},
    {"VipsForeignLoadJxlSource", "jxlload_source", "jxl"},
    {"VipsForeignLoadPpmSource", "ppmload_source", "ppm"},
    {"VipsForeignLoadJp2kSource", "jp2kload_source", "jp2"},
    {"VipsForeignLoadRadSource", "radload_source", "hdr"},
    {"VipsForeignLoadOpenexr", "openexrload", "exr", Input::DescriptorPath, "\x76\x2f\x31\x01"},
    {nullptr, "bmp", "bmp", Input::Qt, "BM"},
    {nullptr, "ico", "ico", Input::Qt, QByteArrayView("\0\0\1\0", 4)},
    {nullptr, "icns", "icns", Input::Qt, "icns"},
    {nullptr, "dds", "dds", Input::Qt, "DDS "},
    {nullptr, "tga", "tga", Input::Qt},
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

bool isTga(int fd, QByteArrayView head, qint64 length) {
    constexpr QByteArrayView footer("TRUEVISION-XFILE.\0", 18);
    char tail[footer.size()];
    if (length >= 44 && ::pread(fd, tail, sizeof tail, length - sizeof tail) == sizeof tail &&
        QByteArrayView(tail, sizeof tail) == footer)
        return true;
    // TGA 1.0 has no magic. Only try the reader for plausible headers;
    // reserved types, invalid depth and impossible data offsets are excluded.
    if (head.size() < 18)
        return false;
    const auto *h = reinterpret_cast<const uchar *>(head.data());
    const int kind = h[2] & ~8;
    const bool depth = (kind == 1 && (h[16] == 8 || h[16] == 16)) ||
                       (kind == 2 && (h[16] == 15 || h[16] == 16 || h[16] == 24 || h[16] == 32)) ||
                       (kind == 3 && (h[16] == 8 || h[16] == 16));
    const int colors = qFromLittleEndian<quint16>(h + 5);
    return depth && !(h[17] & 0xc0) && qFromLittleEndian<quint16>(h + 12) &&
           qFromLittleEndian<quint16>(h + 14) &&
           (kind == 1 ? (h[1] == 1 && colors && h[7] >= 15 && h[7] <= 32) : h[1] == 0) &&
           length > 18 + h[0] + (kind == 1 ? colors * ((h[7] + 7) / 8) : 0);
}

DecodeReport decodeWithQt(const Loader &loader, int input, int output, const DecodeSpec &spec) {
    QFile file;
    if (!file.open(input, QIODevice::ReadOnly) || !file.seek(0))
        return fail(DecodeCode::Unreadable, "Could not open the image descriptor");
    QImageReader reader(&file, loader.operation);
    reader.setAutoDetectImageFormat(false);
    reader.setAutoTransform(true);
    if (!reader.canRead()) {
        const auto code = reader.error() == QImageReader::UnsupportedFormatError
                              ? DecodeCode::Unsupported
                              : DecodeCode::InvalidHeader;
        return fail(code, reader.errorString());
    }
    const auto dimensions = reader.size();
    if (dimensions.isValid() && qint64(dimensions.width()) * dimensions.height() > MaxSourcePixels)
        return fail(DecodeCode::PixelLimit, "Source frame exceeds 64 MiPixels");
    // Some readers cannot report the header dimensions independently (e.g.
    // ICNS). Qt's allocation limit and the helper address-space limit still
    // apply to the first decode, including metadata-only requests.
    QImage pixels;
    if (spec.mode != DecodeSpec::Inspect || !dimensions.isValid()) {
        pixels = reader.read();
        if (pixels.isNull())
            return fail(dimensions.isValid() ? DecodeCode::Decode : DecodeCode::InvalidHeader,
                        reader.errorString());
        if (qint64(pixels.width()) * pixels.height() > MaxSourcePixels)
            return fail(DecodeCode::PixelLimit, "Source frame exceeds 64 MiPixels");
    }
    auto size = pixels.isNull() ? dimensions : pixels.size();
    if (pixels.isNull() && (reader.transformation() & QImageIOHandler::TransformationRotate90))
        size.transpose();
    if (size.isEmpty())
        return fail(DecodeCode::InvalidHeader, "Nonpositive image dimensions");
    DecodeReport report;
    report.source = {QString::fromLatin1(loader.format), size, 1, 1};
    if (spec.mode == DecodeSpec::Inspect)
        return report;
    if (spec.mode == DecodeSpec::FrameAt)
        return fail(DecodeCode::Decode, "Source has no animation timing");
    auto target = spec.bounds;
    if (spec.scale == DecodeSpec::Cover) {
        target = target.scaled(size.boundedTo(target), Qt::KeepAspectRatio).expandedTo({1, 1});
        const auto scaled = pixels.size().scaled(target, Qt::KeepAspectRatioByExpanding);
        // Crop first so a tiny output never allocates an oversized intermediary.
        if (scaled != target) {
            const auto crop = target.scaled(size, Qt::KeepAspectRatio).expandedTo({1, 1});
            pixels = pixels.copy((size.width() - crop.width()) / 2,
                                 (size.height() - crop.height()) / 2, crop.width(), crop.height());
        }
    } else if (spec.scale == DecodeSpec::Stretch) {
        target = target.boundedTo(size);
    } else {
        target = size.scaled(size.boundedTo(target), Qt::KeepAspectRatio).expandedTo({1, 1});
    }
    if (pixels.colorSpace().isValid() && pixels.colorSpace() != QColorSpace::SRgb)
        pixels.convertToColorSpace(QColorSpace::SRgb);
    pixels = pixels.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                 .convertToFormat(QImage::Format_RGBA8888);
    report.output = pixels.size();
    const auto bytes = rgbaBytes(report.output);
    if (bytes <= 0 || report.output.width() > spec.bounds.width() ||
        report.output.height() > spec.bounds.height() || pixels.sizeInBytes() != bytes)
        return fail(DecodeCode::Output, "Output geometry is out of bounds");
    if (!writeAll(output, reinterpret_cast<const char *>(pixels.constBits()), bytes, 0))
        return fail(DecodeCode::Output, "Could not write RGBA pixels");
    return report;
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
        if (loader.type)
            vips_operation_block_set(loader.type, FALSE);
    QImageReader::setAllocationLimit(int(MaxSourcePixels * 4 / (1024 * 1024)));
    return true;
}

DecodeReport decodeImage(int input, int output, const DecodeSpec &spec, qint64 length) {
    if (length <= 0)
        return fail(DecodeCode::InvalidHeader, "Empty source");

    ReadCap cap{input, length};
    auto *custom = vips_source_custom_new();
    Source source(VIPS_SOURCE(custom));
    g_signal_connect(custom, "read", G_CALLBACK(ReadCap::read), &cap);
    g_signal_connect(custom, "seek", G_CALLBACK(ReadCap::seek), &cap);
    char buffer[18];
    const auto prefixSize = ::pread(input, buffer, sizeof buffer, 0);
    if (prefixSize < 0)
        return fail(DecodeCode::Unreadable, "Could not read the image signature");
    const QByteArrayView head(buffer, prefixSize);
    const Loader *loader = nullptr;
    for (const auto &candidate : Loaders) {
        const bool recognized =
            candidate.input == Input::Source
                ? !vips_source_rewind(source.get()) &&
                      vips_foreign_is_a_source(candidate.operation, source.get())
            : !candidate.signature.empty() ? head.startsWith(candidate.signature)
                                           : isTga(input, head, length);
        if (recognized) {
            loader = &candidate;
            break;
        }
        vips_error_clear();
    }
    if (!loader)
        return fail(DecodeCode::Unsupported, "No allowed decoder recognizes this source");
    if (loader->input == Input::Qt)
        return decodeWithQt(*loader, input, output, spec);

    VipsImage *raw = nullptr;
    // OpenEXR has no libvips source loader. The path is generated from the
    // already-validated, sealed descriptor, never from a supplied filename.
    const auto path = "/proc/self/fd/" + QByteArray::number(input);
    const bool descriptorPath = loader->input == Input::DescriptorPath;
    const auto status =
        descriptorPath
            ? vips_call(loader->operation, path.constData(), &raw, "access", VIPS_ACCESS_SEQUENTIAL,
                        "fail_on", VIPS_FAIL_ON_ERROR, nullptr)
            : vips_source_rewind(source.get()) ||
                  vips_call(loader->operation, source.get(), &raw, "access", VIPS_ACCESS_SEQUENTIAL,
                            "fail_on", VIPS_FAIL_ON_ERROR, nullptr);
    if (status)
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
    // JPEG 2000 exposes resolution reductions as pages, not frames.
    const int pages =
        QByteArrayView(loader->format) == "jp2" ? 1 : qMax(1, vips_image_get_n_pages(raw));
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
        const auto crop =
            spec.scale == DecodeSpec::Cover ? VIPS_INTERESTING_CENTRE : VIPS_INTERESTING_NONE;
        const auto scale = spec.scale == DecodeSpec::Stretch ? VIPS_SIZE_FORCE : VIPS_SIZE_DOWN;
        if (descriptorPath
                ? vips_thumbnail_image(image.get(), &raw, target.width(), "height", target.height(),
                                       "crop", crop, "size", scale, nullptr)
                : vips_source_rewind(source.get()) ||
                      vips_thumbnail_source(source.get(), &raw, target.width(), "height",
                                            target.height(), "crop", crop, "size", scale, "fail_on",
                                            VIPS_FAIL_ON_ERROR, "option-string",
                                            options.constData(), nullptr))
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
