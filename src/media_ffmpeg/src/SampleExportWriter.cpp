#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "dvs/media/SampleExportWriter.h"

#include "dvs/media/MediaProbe.h"
#include "dvs/platform/CpuNv12FrameResource.h"
#include "dvs/platform/CpuP010FrameResource.h"
#include "dvs/platform/FrameBudget.h"

#include "AvRaii.h"
#include "SampleExportWriterTestHooks.h"
#include "SoftwareDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

namespace dvs::media {
namespace {

// Component-test seams. The staged-member hook fires after each staged file is closed and before
// read-back validation; the WIC override forces the fallback encoder path deterministically.
std::atomic<testing::SampleExportStagedMemberCallback> stagedMemberCallback{nullptr};
std::atomic<bool> forceWicFallback{false};

constexpr std::array<std::uint8_t, 8> kPngSignature{
    0x89U,
    'P',
    'N',
    'G',
    0x0DU,
    0x0AU,
    0x1AU,
    0x0AU,
};

// E01, kept honest: the exported members are decoded and converted values. The decode normalizes
// to the NV12/P010 display format, swscale converts to RGB24 with the probed matrix and range,
// and the PNG step itself is lossless - the bytes are never the source codec's bit-exact code
// values, and the manifest says so instead of claiming otherwise.
constexpr std::string_view kPixelConversionPolicy =
    "decoded yuv to rgb24 via swscale (probed color matrix and range), lossless PNG; values are "
    "decoded conversion values, not source-codec bit-exact code values";
constexpr std::string_view kExporterVersion = "dvs::media::SampleExportWriter/1";

[[nodiscard]] bool isCanceled(const std::atomic_bool& flag) noexcept {
    return flag.load(std::memory_order_acquire);
}

void removeTree(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

[[nodiscard]] std::string ffmpegError(const int errorCode) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    if (av_strerror(errorCode, buffer, sizeof(buffer)) < 0) {
        return "FFmpeg returned error " + std::to_string(errorCode) + ".";
    }
    return std::string{buffer};
}

[[nodiscard]] std::string hresultMessage(const char* const what, const HRESULT result) {
    std::ostringstream text;
    text << what << " (HRESULT 0x" << std::hex << static_cast<unsigned int>(result) << ").";
    return text.str();
}

[[nodiscard]] application::SampleExportReport
makeReport(const application::SampleExportJob& job,
           const application::SampleExportOutcome outcome,
           std::string detail) {
    application::SampleExportReport report;
    report.requestId = job.requestId;
    report.outcome = outcome;
    report.technicalDetail = std::move(detail);
    return report;
}

// The sample id becomes exactly one path segment of the published directory, so it may not carry
// separators, a leading dot (which would collide with the staging/retired names or mean "." /
// ".."), or characters Windows forbids in portable names.
[[nodiscard]] bool isSafeSampleDirectoryName(const std::string_view name) noexcept {
    if (name.empty() || name.size() > 96U || name.front() == '.') {
        return false;
    }
    for (const char character : name) {
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') || character == '-' ||
                             character == '_' || character == '.';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

// A tightly packed display-space RGB frame: `height` rows of `width` * 3 bytes, no padding.
struct Rgb24Image final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> pixels;
};

[[nodiscard]] int swsColorSpace(const domain::ColorMatrix matrix) noexcept {
    return matrix == domain::ColorMatrix::kBt709 ? SWS_CS_ITU709 : SWS_CS_ITU601;
}

// One conversion per frame, so a fresh context is fine; the flags only govern scaling, and this
// conversion never scales. The destination range is full because RGB24 has no limited variant.
[[nodiscard]] bool convertYuvToRgb24(const AVPixelFormat sourceFormat,
                                     const std::uint32_t width,
                                     const std::uint32_t height,
                                     const std::span<const std::uint8_t> yPlane,
                                     const std::uint32_t yStride,
                                     const std::span<const std::uint8_t> uvPlane,
                                     const std::uint32_t uvStride,
                                     const domain::ColorMetadata& color,
                                     Rgb24Image* const out,
                                     std::string* const error) {
    if (width == 0U || height == 0U || yStride == 0U || uvStride == 0U) {
        *error = "The decoded member frame has a degenerate layout.";
        return false;
    }
    const std::uint64_t rgbStride64 = static_cast<std::uint64_t>(width) * 3ULL;
    if (rgbStride64 > static_cast<std::uint64_t>(INT_MAX) ||
        static_cast<std::uint64_t>(yStride) > static_cast<std::uint64_t>(INT_MAX) ||
        static_cast<std::uint64_t>(uvStride) > static_cast<std::uint64_t>(INT_MAX)) {
        *error = "The decoded member frame's strides exceed FFmpeg limits.";
        return false;
    }
    out->width = width;
    out->height = height;
    const std::size_t rgbStride = static_cast<std::size_t>(rgbStride64);
    out->pixels.assign(static_cast<std::size_t>(height) * rgbStride, std::uint8_t{0U});

    internal::SwsContextPtr scaler{sws_getContext(static_cast<int>(width),
                                                  static_cast<int>(height),
                                                  sourceFormat,
                                                  static_cast<int>(width),
                                                  static_cast<int>(height),
                                                  AV_PIX_FMT_RGB24,
                                                  SWS_BILINEAR,
                                                  nullptr,
                                                  nullptr,
                                                  nullptr)};
    if (scaler == nullptr) {
        *error = "FFmpeg could not create the RGB24 conversion context.";
        return false;
    }
    const int* const coefficients = sws_getCoefficients(swsColorSpace(color.matrix));
    const int sourceFullRange = color.range == domain::ColorRange::kFull ? 1 : 0;
    if (coefficients == nullptr || sws_setColorspaceDetails(scaler.get(),
                                                            coefficients,
                                                            sourceFullRange,
                                                            coefficients,
                                                            /*destinationFullRange=*/1,
                                                            0,
                                                            1 << 16,
                                                            1 << 16) < 0) {
        *error = "FFmpeg could not configure the RGB24 color conversion.";
        return false;
    }
    const std::array<const std::uint8_t*, 4> sourceSlices{
        yPlane.data(),
        uvPlane.data(),
        nullptr,
        nullptr,
    };
    const std::array<int, 4> sourceStrides{
        static_cast<int>(yStride),
        static_cast<int>(uvStride),
        0,
        0,
    };
    std::array<std::uint8_t*, 4> targetSlices{
        out->pixels.data(),
        nullptr,
        nullptr,
        nullptr,
    };
    const std::array<int, 4> targetStrides{
        static_cast<int>(rgbStride),
        0,
        0,
        0,
    };
    const int convertedRows = sws_scale(scaler.get(),
                                        sourceSlices.data(),
                                        sourceStrides.data(),
                                        0,
                                        static_cast<int>(height),
                                        targetSlices.data(),
                                        targetStrides.data());
    if (convertedRows != static_cast<int>(height)) {
        *error = "FFmpeg could not convert the decoded member frame to RGB24.";
        return false;
    }
    return true;
}

[[nodiscard]] bool convertDecodedMemberToRgb24(const application::FrameHandle& handle,
                                               Rgb24Image* const out,
                                               std::string* const error) {
    const auto p010 =
        std::dynamic_pointer_cast<const platform::CpuP010FrameResource>(handle.resource());
    if (p010 != nullptr) {
        const platform::P010FrameLayout& layout = p010->layout();
        return convertYuvToRgb24(AV_PIX_FMT_P010LE,
                                 layout.width,
                                 layout.height,
                                 p010->yPlane(),
                                 layout.yStride,
                                 p010->uvPlane(),
                                 layout.uvStride,
                                 p010->colorMetadata(),
                                 out,
                                 error);
    }
    const auto nv12 =
        std::dynamic_pointer_cast<const platform::CpuNv12FrameResource>(handle.resource());
    if (nv12 != nullptr) {
        const platform::Nv12FrameLayout& layout = nv12->layout();
        return convertYuvToRgb24(AV_PIX_FMT_NV12,
                                 layout.width,
                                 layout.height,
                                 nv12->yPlane(),
                                 layout.yStride,
                                 nv12->uvPlane(),
                                 layout.uvStride,
                                 nv12->colorMetadata(),
                                 out,
                                 error);
    }
    *error = "The decoded member frame is not a CPU NV12/P010 resource.";
    return false;
}

// The rectangle is half-open on the right and bottom edges, in source pixel coordinates. The
// subtraction form of the bounds check cannot overflow: crop.width <= width is exactly what the
// comparison establishes first.
[[nodiscard]] bool cropFitsInside(const std::int64_t width,
                                  const std::int64_t height,
                                  const application::SampleCropRect& crop) noexcept {
    return crop.x >= 0 && crop.y >= 0 && crop.width > 0 && crop.height > 0 &&
           crop.x <= width - crop.width && crop.y <= height - crop.height;
}

[[nodiscard]] bool cropRgb24(const Rgb24Image& source,
                             const application::SampleCropRect& crop,
                             Rgb24Image* const out) {
    if (!cropFitsInside(static_cast<std::int64_t>(source.width),
                        static_cast<std::int64_t>(source.height),
                        crop)) {
        return false;
    }
    out->width = static_cast<std::uint32_t>(crop.width);
    out->height = static_cast<std::uint32_t>(crop.height);
    const std::size_t sourceStride = static_cast<std::size_t>(source.width) * 3U;
    const std::size_t targetStride = static_cast<std::size_t>(out->width) * 3U;
    out->pixels.assign(static_cast<std::size_t>(out->height) * targetStride, std::uint8_t{0U});
    for (std::int64_t row = 0; row < crop.height; ++row) {
        const std::size_t sourceOffset = static_cast<std::size_t>(crop.y + row) * sourceStride +
                                         static_cast<std::size_t>(crop.x) * 3U;
        std::memcpy(out->pixels.data() + static_cast<std::size_t>(row) * targetStride,
                    source.pixels.data() + sourceOffset,
                    targetStride);
    }
    return true;
}

// Encodes lossless PNG through the FFmpeg avcodec png encoder. Selection rationale: the shared
// avcodec library already carries this encoder (its png work needs only the zlib that the build
// links), it accepts RGB24 directly, and it keeps the exporter on the same media stack as the
// decoder - no second imaging dependency.
[[nodiscard]] std::vector<std::uint8_t> encodePngWithAvCodec(const Rgb24Image& image) {
    std::vector<std::uint8_t> png;
    const AVCodec* const codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (codec == nullptr || codec->type != AVMEDIA_TYPE_VIDEO) {
        return png;
    }
    internal::AvCodecContextPtr context{avcodec_alloc_context3(codec)};
    if (context == nullptr) {
        return png;
    }
    context->width = static_cast<int>(image.width);
    context->height = static_cast<int>(image.height);
    context->pix_fmt = AV_PIX_FMT_RGB24;
    context->time_base = AVRational{.num = 1, .den = 1};
    if (avcodec_open2(context.get(), codec, nullptr) < 0) {
        return png;
    }

    internal::AvFramePtr frame{av_frame_alloc()};
    if (frame == nullptr) {
        return png;
    }
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = context->width;
    frame->height = context->height;
    // get_buffer needs the format and geometry set first, and may pad linesize, so the tightly
    // packed rows are copied instead of handing the encoder a pointer into `image`.
    if (av_frame_get_buffer(frame.get(), 0) < 0) {
        return png;
    }
    frame->pts = 0;
    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 3U;
    for (std::uint32_t row = 0U; row < image.height; ++row) {
        std::memcpy(frame->data[0] + static_cast<std::size_t>(row) *
                                         static_cast<std::size_t>(frame->linesize[0]),
                    image.pixels.data() + static_cast<std::size_t>(row) * rowBytes,
                    rowBytes);
    }
    if (avcodec_send_frame(context.get(), frame.get()) < 0) {
        return png;
    }
    av_frame_unref(frame.get());
    if (avcodec_send_frame(context.get(), nullptr) < 0) {
        return png;
    }

    internal::AvPacketPtr packet{av_packet_alloc()};
    if (packet == nullptr) {
        return png;
    }
    for (;;) {
        const int receiveResult = avcodec_receive_packet(context.get(), packet.get());
        if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) {
            break;
        }
        if (receiveResult < 0) {
            png.clear();
            break;
        }
        png.insert(png.end(), packet->data, packet->data + packet->size);
        av_packet_unref(packet.get());
    }
    return png;
}

// Calls CoInitializeEx on the calling thread, tolerating an existing apartment of either model,
// and always balances a successful initialization with CoUninitialize.
class ScopedComApartment final {
public:
    ScopedComApartment() noexcept {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result_ == RPC_E_CHANGED_MODE) {
            result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        }
    }

    ~ScopedComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;

    [[nodiscard]] bool ready() const noexcept {
        return SUCCEEDED(result_);
    }

    [[nodiscard]] std::string failure() const {
        return hresultMessage("COM could not be initialized on the export worker", result_);
    }

private:
    HRESULT result_ = E_FAIL;
};

// Windows Imaging Component fallback for builds whose avcodec carries no png encoder. WIC is
// already part of the operating system, so the fallback adds no product dependency. The PNG
// container is lossless by construction; the pixel format round-trips through 24bppBGR, the one
// 24-bit canonical format WIC's PNG encoder accepts.
[[nodiscard]] std::vector<std::uint8_t> encodePngWithWic(const Rgb24Image& image) {
    std::vector<std::uint8_t> png;
    ScopedComApartment apartment;
    if (!apartment.ready()) {
        return png;
    }
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        return png;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, 1U);
    if (memory == nullptr) {
        return png;
    }
    Microsoft::WRL::ComPtr<IStream> stream;
    // deleteOnRelease = TRUE hands the HGLOBAL's lifetime to the stream.
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        return png;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frameEncode;
    Microsoft::WRL::ComPtr<IPropertyBag2> properties;
    if (SUCCEEDED(result)) {
        result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    }
    if (SUCCEEDED(result)) {
        result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(result)) {
        result = encoder->CreateNewFrame(&frameEncode, &properties);
    }
    if (SUCCEEDED(result)) {
        result = frameEncode->Initialize(properties.Get());
    }
    if (SUCCEEDED(result)) {
        result =
            frameEncode->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height));
    }
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(result)) {
        result = frameEncode->SetPixelFormat(&pixelFormat);
    }
    if (SUCCEEDED(result) && pixelFormat != GUID_WICPixelFormat24bppBGR) {
        result = E_UNEXPECTED;
    }
    std::vector<std::uint8_t> bgr;
    if (SUCCEEDED(result)) {
        const std::size_t stride = static_cast<std::size_t>(image.width) * 3U;
        bgr.assign(static_cast<std::size_t>(image.height) * stride, std::uint8_t{0U});
        for (std::size_t offset = 0U; offset + 2U < bgr.size(); offset += 3U) {
            bgr[offset] = image.pixels[offset + 2U];
            bgr[offset + 1U] = image.pixels[offset + 1U];
            bgr[offset + 2U] = image.pixels[offset];
        }
        result = frameEncode->WritePixels(static_cast<UINT>(image.height),
                                          static_cast<UINT>(stride),
                                          static_cast<UINT>(bgr.size()),
                                          bgr.data());
    }
    if (SUCCEEDED(result)) {
        result = frameEncode->Commit();
    }
    if (SUCCEEDED(result)) {
        result = encoder->Commit();
    }
    if (FAILED(result)) {
        return png;
    }

    STATSTG statistics{};
    LARGE_INTEGER start{};
    ULARGE_INTEGER position{};
    if (FAILED(stream->Stat(&statistics, STATFLAG_NONAME)) || statistics.cbSize.QuadPart <= 0 ||
        statistics.cbSize.QuadPart > (64LL << 20) ||
        FAILED(stream->Seek(start, STREAM_SEEK_SET, &position))) {
        return png;
    }
    const std::size_t size = static_cast<std::size_t>(statistics.cbSize.QuadPart);
    png.resize(size);
    ULONG read = 0U;
    if (FAILED(stream->Read(png.data(), static_cast<ULONG>(size), &read)) ||
        read != static_cast<ULONG>(size)) {
        png.clear();
    }
    return png;
}

enum class PngEncoder : std::uint8_t {
    kAvCodec,
    kWic,
};

[[nodiscard]] const char* pngEncoderName(const PngEncoder encoder) noexcept {
    return encoder == PngEncoder::kWic ? "windows-wic" : "ffmpeg-avcodec";
}

// Runtime probe: opening a tiny RGB24 png encoder context proves the build carries a usable
// avcodec png encoder, without relying on the encoder-list API of a particular FFmpeg version.
[[nodiscard]] bool avCodecPngEncoderAvailable() {
    const AVCodec* const codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (codec == nullptr || codec->type != AVMEDIA_TYPE_VIDEO) {
        return false;
    }
    internal::AvCodecContextPtr probe{avcodec_alloc_context3(codec)};
    if (probe == nullptr) {
        return false;
    }
    probe->width = 16;
    probe->height = 16;
    probe->pix_fmt = AV_PIX_FMT_RGB24;
    probe->time_base = AVRational{.num = 1, .den = 1};
    return avcodec_open2(probe.get(), codec, nullptr) >= 0;
}

[[nodiscard]] std::vector<std::uint8_t> encodePng(const Rgb24Image& image,
                                                  const PngEncoder encoder) {
    return encoder == PngEncoder::kWic ? encodePngWithWic(image) : encodePngWithAvCodec(image);
}

// Removes the staging tree while it exists, so a canceled, failed, or invalidated export leaves
// no half-built sample behind. Released right after the staging tree is renamed into place.
class StagingCleanup final {
public:
    explicit StagingCleanup(std::filesystem::path staging) : staging_(std::move(staging)) {}

    ~StagingCleanup() {
        if (active_) {
            removeTree(staging_);
        }
    }

    StagingCleanup(const StagingCleanup&) = delete;
    StagingCleanup& operator=(const StagingCleanup&) = delete;

    void release() noexcept {
        active_ = false;
    }

private:
    std::filesystem::path staging_;
    bool active_ = true;
};

[[nodiscard]] bool writeBinaryFile(const std::filesystem::path& path,
                                   const std::span<const std::uint8_t> bytes,
                                   std::string* const error) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    if (!stream) {
        *error = "The staged member could not be created for writing.";
        return false;
    }
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    stream.close();
    if (!stream) {
        *error = "The staged member could not be written to disk.";
        return false;
    }
    return true;
}

[[nodiscard]] bool writeTextFile(const std::filesystem::path& path,
                                 const std::string_view text,
                                 std::string* const error) {
    return writeBinaryFile(path,
                           std::span<const std::uint8_t>{
                               reinterpret_cast<const std::uint8_t*>(text.data()), text.size()},
                           error);
}

[[nodiscard]] bool validateStagedPng(const std::filesystem::path& path, std::string* const error) {
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size <= kPngSignature.size()) {
        *error = "A staged member is missing or too small to be a PNG.";
        return false;
    }
    std::ifstream stream{path, std::ios::binary};
    std::array<std::uint8_t, 8> header{};
    stream.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    if (!stream || header != kPngSignature) {
        *error = "A staged member does not carry the PNG signature.";
        return false;
    }
    return true;
}

[[nodiscard]] bool validateStagedManifest(const std::filesystem::path& path,
                                          const std::string& expectedSampleId,
                                          std::string* const error) {
    std::ifstream stream{path, std::ios::binary};
    const std::string text{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    if (!stream && text.empty()) {
        *error = "The staged manifest could not be read back.";
        return false;
    }
    try {
        const nlohmann::json manifest = nlohmann::json::parse(text);
        if (!manifest.is_object() || manifest.value("schema_version", 0) != 1 ||
            manifest.value("sample_id", std::string{}) != expectedSampleId ||
            !manifest.contains("members") || !manifest["members"].contains("input0") ||
            !manifest["members"].contains("gt") || !manifest["members"].contains("input1") ||
            !manifest["members"].contains("prediction")) {
            *error = "The staged manifest does not carry this sample's identity.";
            return false;
        }
    } catch (const nlohmann::json::exception& exception) {
        *error = "The staged manifest is not valid JSON: " + std::string{exception.what()};
        return false;
    }
    return true;
}

// Publishes the validated staging tree under its final name. A previous sample directory is set
// aside and only deleted after the rename succeeded; if the rename fails, the previous sample is
// restored, so a failure never destroys the old sample or half-replaces it.
[[nodiscard]] std::string publishStagedSample(const std::filesystem::path& staging,
                                              const std::filesystem::path& finalDirectory,
                                              const std::filesystem::path& retiredDirectory) {
    std::error_code filesystemError;
    const bool replacing = std::filesystem::exists(finalDirectory, filesystemError);
    if (filesystemError) {
        return "The sample destination could not be inspected: " + filesystemError.message();
    }
    if (replacing) {
        std::filesystem::rename(finalDirectory, retiredDirectory, filesystemError);
        if (filesystemError) {
            return "The previous sample directory could not be set aside: " +
                   filesystemError.message();
        }
    }
    std::filesystem::rename(staging, finalDirectory, filesystemError);
    if (filesystemError) {
        std::error_code restoreIgnored;
        if (replacing) {
            std::filesystem::rename(retiredDirectory, finalDirectory, restoreIgnored);
        }
        return "The validated sample could not be published: " + filesystemError.message();
    }
    if (replacing) {
        removeTree(retiredDirectory);
    }
    return {};
}

[[nodiscard]] std::string mappingOriginName(const domain::MappingOrigin origin) {
    return origin == domain::MappingOrigin::kImportedManifest ? "imported_manifest"
           : origin == domain::MappingOrigin::kManual         ? "manual"
                                                              : "suggestion";
}

// A4 manifest subset, filled in honestly. Content fingerprints (SHA-256 of members and sources)
// are deliberately absent: they belong to E04 lineage work, and a fake or placeholder digest
// would be worse than a missing field.
[[nodiscard]] nlohmann::json buildManifest(const application::SampleExportJob& job,
                                           const char* const selectedEncoder) {
    const application::SampleTripletPlan& plan = job.plan;

    nlohmann::json interpolation = nlohmann::json::object();
    interpolation["input0_gt_index"] = plan.input0GtFrame.value();
    interpolation["target_gt_index"] = plan.targetGtFrame.value();
    interpolation["input1_gt_index"] = plan.input1GtFrame.value();
    interpolation["index_base"] = 0;
    interpolation["target_time_ratio"] =
        nlohmann::json::array({plan.targetRatioNumerator, plan.targetRatioDenominator});
    interpolation["mapping_origin"] = mappingOriginName(plan.mappingOrigin);

    nlohmann::json members = nlohmann::json::object();
    members["input0"] = "frames/input0.png";
    members["gt"] = "frames/gt.png";
    members["input1"] = "frames/input1.png";
    members["prediction"] = "frames/prediction.png";

    nlohmann::json crop = nlohmann::json();
    if (plan.cropRect.has_value()) {
        crop = nlohmann::json{{"x", plan.cropRect->x},
                              {"y", plan.cropRect->y},
                              {"width", plan.cropRect->width},
                              {"height", plan.cropRect->height}};
    }

    nlohmann::json manifest = nlohmann::json::object();
    manifest["schema_version"] = 1;
    manifest["sample_id"] = job.sampleId;
    manifest["interpolation"] = std::move(interpolation);
    manifest["members"] = std::move(members);
    manifest["crop"] = std::move(crop);
    manifest["pixel_conversion_policy"] = std::string{kPixelConversionPolicy};
    manifest["png_encoder"] = selectedEncoder;
    manifest["exporter"] = std::string{kExporterVersion};
    return manifest;
}

// One decoded member. The three GT members are ordered input0 < target < input1 by the planner,
// which is also presentation order, so writing them in this order decodes the GT asset forwards.
struct Member final {
    const char* name = nullptr;
    bool fromCandidate = false;
    std::int64_t frame = 0;
};

[[nodiscard]] std::uint64_t frameBudgetBytes(const domain::MediaDescriptor& descriptor) noexcept {
    const std::uint64_t width = descriptor.extent.width;
    const std::uint64_t height = descriptor.extent.height;
    const std::uint64_t bytesPerSample = descriptor.bitDepth > 8U ? 2ULL : 1ULL;
    // YUV 4:2:0 needs 12 bits per pixel per 8-bit sample; the multiplier of two plus slack keeps
    // one live handle per decoder inside the budget even with alignment and pool retention.
    return width * height * bytesPerSample * 2ULL + (1ULL << 20);
}

void notifyStagedMember(const std::filesystem::path& path) noexcept {
    const auto callback = stagedMemberCallback.load(std::memory_order_acquire);
    if (callback != nullptr) {
        callback(path);
    }
}

} // namespace

application::SampleExportReport
SampleExportWriter::perform(const application::SampleExportJob& job,
                            const std::atomic_bool& cancelRequested) {
    if (job.requestId == application::kInvalidSampleExportRequestId) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "A sample export needs a non-zero request id.");
    }
    if (!isSafeSampleDirectoryName(job.sampleId)) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "The sample id is not a safe single directory name.");
    }
    if (job.gtAssetPath.empty() || job.candidateAssetPath.empty() || job.outputDirectory.empty()) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "A sample export needs both asset paths and an output directory.");
    }
    if (isCanceled(cancelRequested)) {
        return makeReport(job, application::SampleExportOutcome::kCanceled, {});
    }

    std::error_code filesystemError;
    std::filesystem::create_directories(job.outputDirectory, filesystemError);
    if (filesystemError) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "The output directory could not be created: " +
                              filesystemError.message());
    }
    const std::filesystem::path finalDirectory = job.outputDirectory / job.sampleId;
    const std::filesystem::path stagingDirectory =
        job.outputDirectory / (".staging-" + std::to_string(job.requestId));
    const std::filesystem::path retiredDirectory =
        job.outputDirectory / (".retired-" + std::to_string(job.requestId));
    // A previous run of this same request may have died between staging and publication. Both
    // trees below are named by this request, so clearing them cannot touch anyone else's work.
    removeTree(stagingDirectory);
    removeTree(retiredDirectory);
    std::filesystem::create_directories(stagingDirectory / "frames", filesystemError);
    if (filesystemError) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "The staging directory could not be created: " +
                              filesystemError.message());
    }
    StagingCleanup stagingCleanup{stagingDirectory};

    const auto gtDescriptor = MediaProbe::inspect(job.gtAssetPath, domain::SourceId{0U});
    if (!gtDescriptor) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "The GT asset could not be probed: " +
                              gtDescriptor.error().technicalDetail);
    }
    const auto candidateDescriptor =
        MediaProbe::inspect(job.candidateAssetPath, domain::SourceId{1U});
    if (!candidateDescriptor) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "The candidate asset could not be probed: " +
                              candidateDescriptor.error().technicalDetail);
    }

    // The plan is data by the time it reaches the writer, so its invariants are re-checked against
    // the assets that were just probed rather than trusted.
    if (job.plan.input0GtFrame.value() < 0 ||
        job.plan.targetGtFrame.value() <= job.plan.input0GtFrame.value() ||
        job.plan.input1GtFrame.value() <= job.plan.targetGtFrame.value() ||
        job.plan.input1GtFrame.value() >= gtDescriptor.value().frameCount.value) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "The plan's GT frame indices do not form an ordered in-range triplet.");
    }
    if (job.plan.candidateOutputFrame.value() < 0 ||
        job.plan.candidateOutputFrame.value() >= candidateDescriptor.value().frameCount.value) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "The plan's candidate output frame is outside the candidate asset.");
    }
    if (job.plan.cropRect.has_value() &&
        (!cropFitsInside(static_cast<std::int64_t>(gtDescriptor.value().extent.width),
                         static_cast<std::int64_t>(gtDescriptor.value().extent.height),
                         *job.plan.cropRect) ||
         !cropFitsInside(static_cast<std::int64_t>(candidateDescriptor.value().extent.width),
                         static_cast<std::int64_t>(candidateDescriptor.value().extent.height),
                         *job.plan.cropRect))) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "The plan's crop rectangle does not fit both probed source geometries.");
    }

    const PngEncoder encoder =
        forceWicFallback.load(std::memory_order_acquire) || !avCodecPngEncoderAvailable()
            ? PngEncoder::kWic
            : PngEncoder::kAvCodec;

    platform::FrameBudget budget{static_cast<std::size_t>(std::max(
        frameBudgetBytes(gtDescriptor.value()), frameBudgetBytes(candidateDescriptor.value())))};
    internal::SoftwareDecoder gtDecoder{domain::SourceId{0U}, gtDescriptor.value(), budget};
    const domain::Status gtOpened = gtDecoder.open(cancelRequested);
    if (!gtOpened) {
        return makeReport(job,
                          isCanceled(cancelRequested) ? application::SampleExportOutcome::kCanceled
                                                      : application::SampleExportOutcome::kFailed,
                          "The GT asset decoder could not be opened: " +
                              gtOpened.error().technicalDetail);
    }
    internal::SoftwareDecoder candidateDecoder{
        domain::SourceId{1U}, candidateDescriptor.value(), budget};
    const domain::Status candidateOpened = candidateDecoder.open(cancelRequested);
    if (!candidateOpened) {
        return makeReport(job,
                          isCanceled(cancelRequested) ? application::SampleExportOutcome::kCanceled
                                                      : application::SampleExportOutcome::kFailed,
                          "The candidate asset decoder could not be opened: " +
                              candidateOpened.error().technicalDetail);
    }

    const std::array<Member, 4> members{{
        {"input0", false, job.plan.input0GtFrame.value()},
        {"gt", false, job.plan.targetGtFrame.value()},
        {"input1", false, job.plan.input1GtFrame.value()},
        {"prediction", true, job.plan.candidateOutputFrame.value()},
    }};

    std::array<std::filesystem::path, 4> stagedMemberPaths{};
    for (std::size_t index = 0U; index < members.size(); ++index) {
        if (isCanceled(cancelRequested)) {
            return makeReport(job, application::SampleExportOutcome::kCanceled, {});
        }
        const Member& member = members[index];
        internal::SoftwareDecoder& decoder = member.fromCandidate ? candidateDecoder : gtDecoder;
        // Frame identity is the zero-based display-order ordinal; SoftwareDecoder pre-rolls from
        // the preceding keyframe internally and accepts nothing but the indexed presentation
        // timestamp, so no timestamp approximation can substitute a neighbouring frame here.
        const auto decoded = decoder.decodeExact(domain::FrameId{member.frame}, cancelRequested);
        if (!decoded) {
            const bool interrupted = isCanceled(cancelRequested) || decoder.lastDecodeInterrupted();
            return makeReport(job,
                              interrupted ? application::SampleExportOutcome::kCanceled
                                          : application::SampleExportOutcome::kFailed,
                              std::string{"The "} + (member.fromCandidate ? "candidate" : "GT") +
                                  " asset's member frame " + std::to_string(member.frame) +
                                  " could not be decoded: " + decoded.error().technicalDetail);
        }

        Rgb24Image fullFrame;
        std::string detail;
        if (!convertDecodedMemberToRgb24(decoded.value().handle, &fullFrame, &detail)) {
            return makeReport(job,
                              application::SampleExportOutcome::kFailed,
                              std::string{"The member "} + member.name +
                                  " could not be converted: " + detail);
        }
        Rgb24Image exported;
        if (job.plan.cropRect.has_value()) {
            if (!cropRgb24(fullFrame, *job.plan.cropRect, &exported)) {
                return makeReport(job,
                                  application::SampleExportOutcome::kFailed,
                                  std::string{"The member "} + member.name +
                                      " does not contain the plan's crop rectangle.");
            }
        } else {
            exported = std::move(fullFrame);
        }

        const std::vector<std::uint8_t> png = encodePng(exported, encoder);
        if (png.empty()) {
            return makeReport(job,
                              application::SampleExportOutcome::kFailed,
                              std::string{"The member "} + member.name +
                                  " could not be encoded as PNG (" + pngEncoderName(encoder) +
                                  ").");
        }
        stagedMemberPaths[index] =
            stagingDirectory / "frames" / (std::string{member.name} + ".png");
        if (!writeBinaryFile(stagedMemberPaths[index], png, &detail)) {
            return makeReport(job,
                              application::SampleExportOutcome::kFailed,
                              std::string{"The member "} + member.name +
                                  " could not be staged: " + detail);
        }
        notifyStagedMember(stagedMemberPaths[index]);
    }

    const std::filesystem::path stagedManifestPath = stagingDirectory / "manifest.json";
    std::string detail;
    if (!writeTextFile(stagedManifestPath,
                       buildManifest(job, pngEncoderName(encoder)).dump(2) + "\n",
                       &detail)) {
        return makeReport(job, application::SampleExportOutcome::kFailed, detail);
    }
    notifyStagedMember(stagedManifestPath);

    // Read every artifact back before anything is published: a staged path could already hold an
    // older copy, and a publish command would ship that copy without complaining.
    for (const std::filesystem::path& memberPath : stagedMemberPaths) {
        if (!validateStagedPng(memberPath, &detail)) {
            return makeReport(job,
                              application::SampleExportOutcome::kFailed,
                              "The staged sample failed read-back validation: " + detail);
        }
    }
    if (!validateStagedManifest(stagedManifestPath, job.sampleId, &detail)) {
        return makeReport(job,
                          application::SampleExportOutcome::kFailed,
                          "The staged sample failed read-back validation: " + detail);
    }
    if (isCanceled(cancelRequested)) {
        return makeReport(job, application::SampleExportOutcome::kCanceled, {});
    }

    const std::string publishError =
        publishStagedSample(stagingDirectory, finalDirectory, retiredDirectory);
    if (!publishError.empty()) {
        return makeReport(job, application::SampleExportOutcome::kFailed, publishError);
    }
    stagingCleanup.release();

    application::SampleExportReport report =
        makeReport(job, application::SampleExportOutcome::kCompleted, {});
    report.publishedMembers.reserve(stagedMemberPaths.size());
    for (std::size_t index = 0U; index < stagedMemberPaths.size(); ++index) {
        report.publishedMembers.push_back(finalDirectory / "frames" /
                                          (std::string{members[index].name} + ".png"));
    }
    report.manifestPath = finalDirectory / "manifest.json";
    return report;
}

namespace testing {

ScopedSampleExportStagedMemberHook::ScopedSampleExportStagedMemberHook(
    const SampleExportStagedMemberCallback callback) noexcept
    : previous_(stagedMemberCallback.exchange(callback, std::memory_order_acq_rel)) {}

ScopedSampleExportStagedMemberHook::~ScopedSampleExportStagedMemberHook() {
    stagedMemberCallback.store(previous_, std::memory_order_release);
}

ScopedSampleExportWicFallback::ScopedSampleExportWicFallback() noexcept {
    forceWicFallback.store(true, std::memory_order_release);
}

ScopedSampleExportWicFallback::~ScopedSampleExportWicFallback() {
    forceWicFallback.store(false, std::memory_order_release);
}

} // namespace testing

} // namespace dvs::media
