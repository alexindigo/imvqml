// SPDX-License-Identifier: LGPL-3.0-only
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <unistd.h>
#include <vector>
#include <webp/decode.h>
#include <webp/encode.h>
#include <zstd.h>

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "vendor/qoi.h"

namespace {
constexpr const char *CodecNames[]{"zstd-raw", "qoi", "lossless-webp"};
constexpr const char *Patterns[]{"icon-alpha", "gradient", "detail"};
constexpr int Samples = 31;

QByteArray pattern(int w, int h, int shape) {
    QByteArray pixels(w * h * 4, Qt::Uninitialized);
    std::mt19937 random(2712);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            auto *p = reinterpret_cast<unsigned char *>(pixels.data() + (y * w + x) * 4);
            if (shape == 0) {
                const double r = std::hypot(2.0 * x / w - 1.0, 2.0 * y / h - 1.0);
                p[0] = x < w / 2 ? 215 : 235;
                p[1] = y < h / 2 ? 35 : 84;
                p[2] = x < w / 2 ? 150 : 202;
                p[3] = std::clamp(int((0.8 - r) * 2000), 1, 255);
            } else if (shape == 1) {
                p[0] = x * 200 / w + 30;
                p[1] = y * 184 / h + 12;
                p[2] = (x + y) * 246 / (w + h);
                p[3] = 255;
            } else {
                const double s =
                    std::sin(x * 0.024 + std::sin(y * 0.012)) * 70 + std::cos(y * 0.033) * 38;
                const int noise = int(random() % 37) - 18;
                p[0] = std::clamp(int(128 + s + noise), 0, 255);
                p[1] = std::clamp(int(111 + s * 0.76 + noise), 0, 255);
                p[2] = std::clamp(int(100 - s * 0.61 + noise), 0, 255);
                p[3] = 255;
            }
        }
    }
    return pixels;
}

QByteArray encode(int codec, const QByteArray &raw, int w, int h) {
    if (codec == 0) {
        QByteArray out(ZSTD_compressBound(raw.size()), Qt::Uninitialized);
        const auto length = ZSTD_compress(out.data(), out.size(), raw.constData(), raw.size(), 3);
        if (ZSTD_isError(length))
            return {};
        out.resize(length);
        return out;
    }
    if (codec == 1) {
        int length = 0;
        const qoi_desc meta{unsigned(w), unsigned(h), 4, QOI_SRGB};
        void *data = qoi_encode(raw.constData(), &meta, &length);
        QByteArray out(static_cast<const char *>(data), length);
        std::free(data);
        return out;
    }
    uint8_t *data = nullptr;
    const auto length = WebPEncodeLosslessRGBA(reinterpret_cast<const uint8_t *>(raw.constData()),
                                               w, h, w * 4, &data);
    QByteArray out(reinterpret_cast<const char *>(data), length);
    WebPFree(data);
    return out;
}

struct Raw {
    QByteArray bytes;
    std::unique_ptr<void, decltype(&std::free)> qoi{nullptr, std::free};
    const void *data() const { return qoi ? qoi.get() : bytes.constData(); }
};

bool decode(Raw &out, int codec, const QByteArray &encoded, int w, int h) {
    if (codec == 1) {
        qoi_desc meta{};
        out.qoi.reset(qoi_decode(encoded.constData(), encoded.size(), &meta, 4));
        return out.qoi && meta.width == unsigned(w) && meta.height == unsigned(h);
    }
    out.bytes.resize(w * h * 4);
    if (codec == 0)
        return ZSTD_decompress(out.bytes.data(), out.bytes.size(), encoded.constData(),
                               encoded.size()) == size_t(out.bytes.size());
    return WebPDecodeRGBAInto(reinterpret_cast<const uint8_t *>(encoded.constData()),
                              encoded.size(), reinterpret_cast<uint8_t *>(out.bytes.data()),
                              out.bytes.size(), w * 4);
}

int run(QOpenGLFunctions &gl, const QString &dir, int w, int h, int shape) {
    const auto image = pattern(w, h, shape);
    std::array<QString, 3> files;
    std::array<qint64, 3> lengths{};
    for (int i = 0; i < 3; ++i) {
        files[i] = dir + '/' + QString::fromLatin1(CodecNames[i]);
        QFile file(files[i]);
        const auto compressed = encode(i, image, w, h);
        Raw check;
        if (compressed.isEmpty() || !decode(check, i, compressed, w, h) ||
            std::memcmp(check.data(), image.data(), image.size()) != 0 ||
            !file.open(QIODevice::WriteOnly) || file.write(compressed) != compressed.size() ||
            !file.flush() || ::fsync(file.handle()) != 0)
            return 1;
        lengths[i] = compressed.size();
    }

    for (bool cold : {false, true}) {
        std::array<std::vector<qint64>, 3> timings;
        for (int iteration = -3; iteration < Samples; ++iteration) {
            for (int j = 0; j < 3; ++j) {
                const int codec = (iteration + j + 3) % 3;
                if (cold) {
                    QFile drop(files[codec]);
                    if (!drop.open(QIODevice::ReadOnly) ||
                        ::posix_fadvise(drop.handle(), 0, 0, POSIX_FADV_DONTNEED))
                        return 2;
                }
                QElapsedTimer timer;
                timer.start();
                QFile file(files[codec]);
                if (!file.open(QIODevice::ReadOnly))
                    return 3;
                auto encoded = file.readAll();
                Raw pixels;
                if (encoded.size() != lengths[codec] || !decode(pixels, codec, encoded, w, h))
                    return 4;
                GLuint texture = 0;
                gl.glGenTextures(1, &texture);
                gl.glBindTexture(GL_TEXTURE_2D, texture);
                gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                pixels.data());
                gl.glFinish();
                const auto ns = timer.nsecsElapsed();
                if (!texture || gl.glGetError() != GL_NO_ERROR)
                    return 5;
                gl.glDeleteTextures(1, &texture);
                if (iteration >= 0)
                    timings[codec].push_back(ns);
            }
        }
        for (int codec = 0; codec < 3; ++codec) {
            auto &sample = timings[codec];
            std::sort(sample.begin(), sample.end());
            std::printf("%s,%dx%d,%s,%s,%lld,%.3f,%.3f\n", Patterns[shape], w, h,
                        cold ? "cold" : "warm", CodecNames[codec],
                        static_cast<long long>(lengths[codec]), sample[Samples / 2] / 1000.0,
                        sample[Samples * 9 / 10] / 1000.0);
        }
    }
    return 0;
}
} // namespace

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (!qEnvironmentVariableIsSet("TMPDIR") || QGuiApplication::platformName() != "wayland")
        return 6;
    QOpenGLContext context;
    if (!context.create())
        return 7;
    QOffscreenSurface surface;
    surface.setFormat(context.format());
    surface.create();
    if (!context.makeCurrent(&surface))
        return 8;
    auto &gl = *context.functions();
    std::printf("renderer,%s\n", gl.glGetString(GL_RENDERER));
    std::printf("fixture,size,lane,codec,bytes,median-us,p90-us\n");
    QTemporaryDir directory(qEnvironmentVariable("TMPDIR") + "/store-benchmark-XXXXXX");
    if (!directory.isValid())
        return 9;
    for (int i = 0; i < 3; ++i) {
        const QSize sizes[]{{256, 256}, {640, 360}, {1920, 1080}};
        if (const int status = run(gl, directory.path(), sizes[i].width(), sizes[i].height(), i))
            return 10 + status;
    }
    return 0;
}
