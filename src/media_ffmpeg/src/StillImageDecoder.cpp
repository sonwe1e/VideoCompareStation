#include "dvs/media/StillImageDecoder.h"

#include "AvRaii.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_id.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace dvs::media {
namespace {

void setError(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message != nullptr ? message : "Unknown image decode failure.";
    }
}

[[nodiscard]] AVCodecID sniffCodecId(const std::uint8_t* data, const std::size_t size) noexcept {
    if (size >= 8U && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return AV_CODEC_ID_PNG;
    }
    if (size >= 3U && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return AV_CODEC_ID_MJPEG;
    }
    if (size >= 2U && data[0] == 'B' && data[1] == 'M') {
        return AV_CODEC_ID_BMP;
    }
    if (size >= 6U &&
        (std::memcmp(data, "GIF87a", 6) == 0 || std::memcmp(data, "GIF89a", 6) == 0)) {
        return AV_CODEC_ID_GIF;
    }
    if (size >= 12U && std::memcmp(data, "RIFF", 4) == 0 && std::memcmp(data + 8, "WEBP", 4) == 0) {
        return AV_CODEC_ID_WEBP;
    }
    if (size >= 4U && data[0] == 'I' && data[1] == 'I' && data[2] == 0x2A && data[3] == 0x00) {
        return AV_CODEC_ID_TIFF;
    }
    if (size >= 4U && data[0] == 'M' && data[1] == 'M' && data[2] == 0x00 && data[3] == 0x2A) {
        return AV_CODEC_ID_TIFF;
    }
    return AV_CODEC_ID_NONE;
}

bool convertFrameToRgba(const AVFrame* frame, StillImage* image, std::string* error) {
    const int width = frame->width;
    const int height = frame->height;
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        setError(error, "Image dimensions are empty or too large.");
        return false;
    }

    const auto rowBytes = static_cast<std::size_t>(width) * 4U;
    const auto totalBytes = rowBytes * static_cast<std::size_t>(height);
    if (totalBytes > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        setError(error, "Image is too large to convert.");
        return false;
    }

    std::vector<std::uint8_t> rgba(totalBytes);
    SwsContext* const scaler = sws_getContext(width,
                                              height,
                                              static_cast<AVPixelFormat>(frame->format),
                                              width,
                                              height,
                                              AV_PIX_FMT_RGBA,
                                              SWS_BILINEAR,
                                              nullptr,
                                              nullptr,
                                              nullptr);
    if (scaler == nullptr) {
        setError(error, "Could not create image color converter.");
        return false;
    }

    std::uint8_t* destination[4] = {rgba.data(), nullptr, nullptr, nullptr};
    const int destinationStride[4] = {static_cast<int>(rowBytes), 0, 0, 0};
    const int scaled =
        sws_scale(scaler, frame->data, frame->linesize, 0, height, destination, destinationStride);
    sws_freeContext(scaler);
    if (scaled != height) {
        setError(error, "Image color conversion failed.");
        return false;
    }

    image->width = width;
    image->height = height;
    image->rgba = std::move(rgba);
    return true;
}

struct MemoryReader final {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    std::size_t offset = 0;
};

int readMemoryPacket(void* opaque, std::uint8_t* buffer, const int bufferSize) {
    auto* reader = static_cast<MemoryReader*>(opaque);
    if (reader == nullptr || buffer == nullptr || bufferSize <= 0 ||
        reader->offset >= reader->size) {
        return AVERROR_EOF;
    }
    const std::size_t remaining = reader->size - reader->offset;
    const auto requested = static_cast<std::size_t>(bufferSize);
    const std::size_t toRead = std::min(requested, remaining);
    std::memcpy(buffer, reader->data + reader->offset, toRead);
    reader->offset += toRead;
    return static_cast<int>(toRead);
}

int64_t seekMemoryPacket(void* opaque, const int64_t offset, const int whence) {
    auto* reader = static_cast<MemoryReader*>(opaque);
    if (reader == nullptr) {
        return AVERROR(EINVAL);
    }
    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(reader->size);
    }
    int64_t origin = 0;
    if (whence == SEEK_CUR) {
        origin = static_cast<int64_t>(reader->offset);
    } else if (whence == SEEK_END) {
        origin = static_cast<int64_t>(reader->size);
    } else if (whence != SEEK_SET) {
        return AVERROR(EINVAL);
    }
    const int64_t next = origin + offset;
    if (next < 0 || next > static_cast<int64_t>(reader->size)) {
        return AVERROR(EINVAL);
    }
    reader->offset = static_cast<std::size_t>(next);
    return next;
}

bool decodeWholeBuffer(const std::uint8_t* data,
                       const std::size_t size,
                       StillImage* image,
                       std::string* error) {
    const AVCodecID codecId = sniffCodecId(data, size);
    if (codecId == AV_CODEC_ID_NONE) {
        setError(error, "Unsupported still-image format.");
        return false;
    }
    const AVCodec* const codec = avcodec_find_decoder(codecId);
    if (codec == nullptr) {
        setError(error, "No decoder is available for this image format.");
        return false;
    }

    internal::AvCodecContextPtr codecContext{avcodec_alloc_context3(codec)};
    if (!codecContext) {
        setError(error, "Could not allocate image decoder.");
        return false;
    }
    if (avcodec_open2(codecContext.get(), codec, nullptr) < 0) {
        setError(error, "Could not open image decoder.");
        return false;
    }

    internal::AvPacketPtr packet{av_packet_alloc()};
    internal::AvFramePtr frame{av_frame_alloc()};
    if (!packet || !frame) {
        setError(error, "Could not allocate image decode buffers.");
        return false;
    }

    // av_new_packet + memcpy keeps decoder-owned packet storage independent of the caller.
    if (av_new_packet(packet.get(), static_cast<int>(size)) < 0) {
        setError(error, "Could not allocate image packet.");
        return false;
    }
    std::memcpy(packet->data, data, size);

    if (avcodec_send_packet(codecContext.get(), packet.get()) < 0) {
        setError(error, "Image packet could not be submitted to the decoder.");
        return false;
    }
    const int receive = avcodec_receive_frame(codecContext.get(), frame.get());
    if (receive != 0) {
        setError(error, "Image frame could not be decoded.");
        return false;
    }
    return convertFrameToRgba(frame.get(), image, error);
}

} // namespace

bool decodeStillImageBytes(const std::uint8_t* data,
                           const std::size_t size,
                           StillImage* image,
                           std::string* error) {
    if (image == nullptr || data == nullptr || size == 0U) {
        setError(error, "Invalid still-image request.");
        return false;
    }
    if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        setError(error, "Image file is too large.");
        return false;
    }
    return decodeWholeBuffer(data, size, image, error);
}

bool decodeStillImageFile(const std::string& path, StillImage* image, std::string* error) {
    if (image == nullptr || path.empty()) {
        setError(error, "Invalid still-image request.");
        return false;
    }

    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
        setError(error, "Could not open image container.");
        return false;
    }
    internal::AvFormatContextPtr formatGuard{format};
    if (avformat_find_stream_info(format, nullptr) < 0) {
        setError(error, "Could not read image stream metadata.");
        return false;
    }
    const int streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (streamIndex < 0) {
        setError(error, "No decodable image stream was found.");
        return false;
    }
    AVStream* const stream = format->streams[streamIndex];
    const AVCodec* const codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (codec == nullptr) {
        setError(error, "No decoder is available for this image format.");
        return false;
    }
    internal::AvCodecContextPtr codecContext{avcodec_alloc_context3(codec)};
    if (!codecContext || avcodec_parameters_to_context(codecContext.get(), stream->codecpar) < 0 ||
        avcodec_open2(codecContext.get(), codec, nullptr) < 0) {
        setError(error, "Could not open image decoder.");
        return false;
    }
    internal::AvPacketPtr packet{av_packet_alloc()};
    internal::AvFramePtr frame{av_frame_alloc()};
    if (!packet || !frame) {
        setError(error, "Could not allocate image decode buffers.");
        return false;
    }
    bool received = false;
    while (av_read_frame(format, packet.get()) >= 0) {
        if (packet->stream_index != streamIndex) {
            av_packet_unref(packet.get());
            continue;
        }
        if (avcodec_send_packet(codecContext.get(), packet.get()) < 0) {
            av_packet_unref(packet.get());
            setError(error, "Image packet could not be submitted to the decoder.");
            return false;
        }
        av_packet_unref(packet.get());
        if (avcodec_receive_frame(codecContext.get(), frame.get()) == 0) {
            received = true;
            break;
        }
    }
    if (!received) {
        static_cast<void>(avcodec_send_packet(codecContext.get(), nullptr));
        if (avcodec_receive_frame(codecContext.get(), frame.get()) != 0) {
            setError(error, "Image frame could not be decoded.");
            return false;
        }
    }
    return convertFrameToRgba(frame.get(), image, error);
}

bool probeStillImageBytes(
    const std::uint8_t* data, const std::size_t size, int* width, int* height, std::string* error) {
    if (data == nullptr || size == 0U || width == nullptr || height == nullptr) {
        setError(error, "Invalid still-image probe request.");
        return false;
    }
    if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        setError(error, "Image file is too large.");
        return false;
    }

    MemoryReader reader{data, size, 0};
    constexpr int kIoBufferSize = 4096;
    auto* ioBuffer = static_cast<std::uint8_t*>(av_malloc(kIoBufferSize));
    if (ioBuffer == nullptr) {
        setError(error, "Could not allocate image metadata buffer.");
        return false;
    }
    AVIOContext* avio = avio_alloc_context(
        ioBuffer, kIoBufferSize, 0, &reader, readMemoryPacket, nullptr, seekMemoryPacket);
    if (avio == nullptr) {
        av_free(ioBuffer);
        setError(error, "Could not allocate image metadata reader.");
        return false;
    }

    AVFormatContext* format = avformat_alloc_context();
    if (format == nullptr) {
        avio_context_free(&avio);
        setError(error, "Could not allocate image metadata context.");
        return false;
    }
    format->pb = avio;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;
    const int openResult = avformat_open_input(&format, nullptr, nullptr, nullptr);
    if (openResult < 0) {
        avio_context_free(&avio);
        setError(error, "Could not read image container metadata.");
        return false;
    }
    const bool streamInfoOk = avformat_find_stream_info(format, nullptr) >= 0;
    int streamIndex = -1;
    if (streamInfoOk) {
        streamIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }
    bool ok = false;
    if (streamIndex >= 0 && format->streams != nullptr && format->streams[streamIndex] != nullptr) {
        const AVCodecParameters* const parameters = format->streams[streamIndex]->codecpar;
        if (parameters != nullptr && parameters->width > 0 && parameters->height > 0) {
            *width = parameters->width;
            *height = parameters->height;
            ok = true;
        }
    }
    avformat_close_input(&format);
    avio_context_free(&avio);
    if (!ok) {
        setError(error, "Could not read image dimensions from the header.");
    }
    return ok;
}

} // namespace dvs::media
