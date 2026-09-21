#pragma once

#include <QByteArray>
#include <QSize>

#include <cstdint>

namespace dvs::ui {

// Format-level dimension probe used before handing bytes to a full decoder. It covers the
// same common still-image containers as the app file filters and never allocates pixel
// buffers. An injected process-wide FFmpeg probe remains the authority for formats this
// lightweight fallback does not know.
[[nodiscard]] inline quint32 readBigEndian32(const uchar* data) noexcept {
    return (static_cast<quint32>(data[0]) << 24U) | (static_cast<quint32>(data[1]) << 16U) |
           (static_cast<quint32>(data[2]) << 8U) | static_cast<quint32>(data[3]);
}

[[nodiscard]] inline quint32 readLittleEndian32(const uchar* data) noexcept {
    return static_cast<quint32>(data[0]) | (static_cast<quint32>(data[1]) << 8U) |
           (static_cast<quint32>(data[2]) << 16U) | (static_cast<quint32>(data[3]) << 24U);
}

[[nodiscard]] inline quint16 readLittleEndian16(const uchar* data) noexcept {
    return static_cast<quint16>(static_cast<quint16>(data[0]) |
                                (static_cast<quint16>(data[1]) << 8U));
}

[[nodiscard]] inline bool probePngSize(const QByteArray& bytes, QSize* size) {
    constexpr int kPngSignatureLength = 8;
    if (bytes.size() < 24 || static_cast<uchar>(bytes[0]) != 0x89 || bytes[1] != 'P' ||
        bytes[2] != 'N' || bytes[3] != 'G') {
        return false;
    }
    static_cast<void>(kPngSignatureLength);
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
    const quint32 width = readBigEndian32(data + 16);
    const quint32 height = readBigEndian32(data + 20);
    if (width == 0U || height == 0U || width > 1000000U || height > 1000000U) {
        return false;
    }
    *size = QSize(static_cast<int>(width), static_cast<int>(height));
    return true;
}

[[nodiscard]] inline bool probeJpegSize(const QByteArray& bytes, QSize* size) {
    if (bytes.size() < 4 || static_cast<uchar>(bytes[0]) != 0xFF ||
        static_cast<uchar>(bytes[1]) != 0xD8) {
        return false;
    }
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
    qsizetype offset = 2;
    while (offset + 9 < bytes.size()) {
        if (data[offset] != 0xFF) {
            ++offset;
            continue;
        }
        while (offset < bytes.size() && data[offset] == 0xFF) {
            ++offset;
        }
        if (offset >= bytes.size()) {
            break;
        }
        const uchar marker = data[offset++];
        if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
            (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }
        if (offset + 1 >= bytes.size()) {
            break;
        }
        const quint16 segmentLength = static_cast<quint16>(
            (static_cast<quint16>(data[offset]) << 8U) | static_cast<quint16>(data[offset + 1]));
        if (segmentLength < 2 || offset + segmentLength > bytes.size()) {
            break;
        }
        const bool startOfFrame =
            (marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC5 && marker <= 0xC7) ||
            (marker >= 0xC9 && marker <= 0xCB) || (marker >= 0xCD && marker <= 0xCF);
        if (startOfFrame) {
            if (segmentLength < 7) {
                return false;
            }
            const quint32 height =
                static_cast<quint32>((static_cast<quint16>(data[offset + 3]) << 8U) |
                                     static_cast<quint16>(data[offset + 4]));
            const quint32 width =
                static_cast<quint32>((static_cast<quint16>(data[offset + 5]) << 8U) |
                                     static_cast<quint16>(data[offset + 6]));
            if (width == 0U || height == 0U) {
                return false;
            }
            *size = QSize(static_cast<int>(width), static_cast<int>(height));
            return true;
        }
        offset += segmentLength;
    }
    return false;
}

[[nodiscard]] inline bool probeGifSize(const QByteArray& bytes, QSize* size) {
    if (bytes.size() < 10 || !bytes.startsWith("GIF87a") && !bytes.startsWith("GIF89a")) {
        return false;
    }
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
    const quint16 width = readLittleEndian16(data + 6);
    const quint16 height = readLittleEndian16(data + 8);
    if (width == 0U || height == 0U) {
        return false;
    }
    *size = QSize(width, height);
    return true;
}

[[nodiscard]] inline bool probeBmpSize(const QByteArray& bytes, QSize* size) {
    if (bytes.size() < 26 || bytes[0] != 'B' || bytes[1] != 'M') {
        return false;
    }
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
    const auto width = static_cast<qint32>(readLittleEndian32(data + 18));
    const auto height = static_cast<qint32>(readLittleEndian32(data + 22));
    const qint32 absWidth = width < 0 ? -width : width;
    const qint32 absHeight = height < 0 ? -height : height;
    if (absWidth <= 0 || absHeight <= 0) {
        return false;
    }
    *size = QSize(absWidth, absHeight);
    return true;
}

[[nodiscard]] inline bool probeWebpSize(const QByteArray& bytes, QSize* size) {
    if (bytes.size() < 30 || !bytes.startsWith("RIFF") || bytes.mid(8, 4) != "WEBP") {
        return false;
    }
    const auto* data = reinterpret_cast<const uchar*>(bytes.constData());
    const QByteArray format = bytes.mid(12, 4);
    if (format == "VP8X") {
        const quint32 width = 1U + static_cast<quint32>(data[24]) |
                              (static_cast<quint32>(data[25]) << 8U) |
                              (static_cast<quint32>(data[26]) << 16U);
        const quint32 height = 1U + static_cast<quint32>(data[27]) |
                               (static_cast<quint32>(data[28]) << 8U) |
                               (static_cast<quint32>(data[29]) << 16U);
        *size = QSize(static_cast<int>(width), static_cast<int>(height));
        return true;
    }
    if (format == "VP8 " && bytes.size() >= 30 && data[23] == 0x9D && data[24] == 0x01 &&
        data[25] == 0x2A) {
        const quint32 width = static_cast<quint32>(
            (static_cast<quint16>(data[26]) | (static_cast<quint16>(data[27]) << 8U)) & 0x3FFFU);
        const quint32 height = static_cast<quint32>(
            (static_cast<quint16>(data[28]) | (static_cast<quint16>(data[29]) << 8U)) & 0x3FFFU);
        if (width > 0U && height > 0U) {
            *size = QSize(static_cast<int>(width), static_cast<int>(height));
            return true;
        }
    }
    if (format == "VP8L" && bytes.size() >= 25 && data[20] == 0x2F) {
        const quint32 width = 1U + (static_cast<quint32>(data[21]) |
                                    ((static_cast<quint32>(data[22]) & 0x3FU) << 8U));
        const quint32 height =
            1U + ((static_cast<quint32>(data[22]) >> 6U) | (static_cast<quint32>(data[23]) << 2U) |
                  ((static_cast<quint32>(data[24]) & 0x0FU) << 10U));
        *size = QSize(static_cast<int>(width), static_cast<int>(height));
        return true;
    }
    return false;
}

[[nodiscard]] inline bool probeImageHeader(const QByteArray& bytes, QSize* size) {
    if (bytes.isEmpty() || size == nullptr) {
        return false;
    }
    return probePngSize(bytes, size) || probeJpegSize(bytes, size) || probeGifSize(bytes, size) ||
           probeBmpSize(bytes, size) || probeWebpSize(bytes, size);
}

} // namespace dvs::ui
