#include "dvs/media/ImageColorProfileProbe.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <map>
#include <utility>
#include <zlib.h>

namespace dvs::media {
namespace {

// A profile payload is metadata, not pixel data. The cap keeps a hostile zlib stream or a
// corrupted chunk length from turning a probe into an unbounded allocation.
constexpr std::size_t kMaximumProfileBytes = std::size_t{16} * 1024U * 1024U;
constexpr std::size_t kInflateBufferBytes = std::size_t{64} * 1024U;
constexpr int kPngSignatureBytes = 8;

[[nodiscard]] std::uint16_t readBig16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

[[nodiscard]] std::uint32_t readBig32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) | static_cast<std::uint32_t>(data[3]);
}

[[nodiscard]] std::uint16_t readLittle16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0] | (static_cast<std::uint16_t>(data[1]) << 8U));
}

[[nodiscard]] std::uint32_t readLittle32(const std::uint8_t* data) noexcept {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

void setError(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

[[nodiscard]] bool inflateProfile(const std::uint8_t* compressed,
                                  const std::size_t compressedSize,
                                  std::vector<std::uint8_t>* profile,
                                  std::string* error) {
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        setError(error, "Could not initialise the PNG profile decompressor.");
        return false;
    }
    stream.next_in = const_cast<Bytef*>(compressed);
    stream.avail_in = static_cast<uInt>(compressedSize);
    std::array<std::uint8_t, kInflateBufferBytes> buffer{};
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        result = inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = buffer.size() - stream.avail_out;
        if (produced > 0U) {
            if (profile->size() + produced > kMaximumProfileBytes) {
                inflateEnd(&stream);
                setError(error, "Embedded ICC profile exceeds the size cap.");
                return false;
            }
            profile->insert(profile->end(),
                            buffer.begin(),
                            buffer.begin() + static_cast<std::ptrdiff_t>(produced));
        }
        if (result == Z_STREAM_END) {
            break;
        }
        if (result == Z_BUF_ERROR && stream.avail_in == 0U) {
            break;
        }
        if (result != Z_OK) {
            inflateEnd(&stream);
            setError(error, "Embedded PNG ICC profile is not a valid zlib stream.");
            return false;
        }
    }
    inflateEnd(&stream);
    if (result != Z_STREAM_END && result != Z_OK) {
        setError(error, "Embedded PNG ICC profile could not be decompressed.");
        return false;
    }
    return true;
}

// PNG: iterate chunks, inflate the iCCP payload (profile name, compression method, zlib data).
[[nodiscard]] bool extractPngProfile(const std::uint8_t* data,
                                     const std::size_t size,
                                     std::vector<std::uint8_t>* profile,
                                     bool* profileDeclared,
                                     std::string* error) {
    static constexpr std::array<std::uint8_t, 8> kSignature{
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < kPngSignatureBytes || std::memcmp(data, kSignature.data(), kSignature.size()) != 0) {
        return false;
    }
    std::size_t offset = kPngSignatureBytes;
    while (offset + 12U <= size) {
        const std::uint32_t length = readBig32(data + offset);
        const std::uint8_t* const type = data + offset + 4U;
        if (std::memcmp(type, "iCCP", 4) == 0) {
            *profileDeclared = true;
            const std::size_t payloadOffset = offset + 8U;
            if (length > size - payloadOffset - 4U) {
                setError(error, "PNG iCCP chunk length exceeds the file.");
                return false;
            }
            const std::uint8_t* const payload = data + payloadOffset;
            std::size_t nameLength = 0U;
            while (nameLength < length && nameLength < 80U && payload[nameLength] != 0U) {
                ++nameLength;
            }
            if (nameLength + 2U > length) {
                setError(error, "PNG iCCP chunk has no complete profile header.");
                return false;
            }
            if (payload[nameLength + 1U] != 0U) {
                setError(error, "PNG iCCP chunk uses an unsupported compression method.");
                return false;
            }
            const std::size_t compressedSize = length - nameLength - 2U;
            if (!inflateProfile(payload + nameLength + 2U, compressedSize, profile, error)) {
                return false;
            }
            return !profile->empty();
        }
        if (length > size - offset - 12U) {
            return false;
        }
        offset += 12U + length;
    }
    return false;
}

// JPEG: concatenate APP2 "ICC_PROFILE" segments by sequence number. Scan stops at SOS/EOI
// because no metadata after the scan start is part of the still-image profile contract.
[[nodiscard]] bool extractJpegProfile(const std::uint8_t* data,
                                      const std::size_t size,
                                      std::vector<std::uint8_t>* profile,
                                      bool* profileDeclared,
                                      std::string* error) {
    if (size < 4U || data[0] != 0xFFU || data[1] != 0xD8U) {
        return false;
    }
    std::map<int, std::vector<std::uint8_t>> segments;
    int declaredCount = 0;
    std::size_t offset = 2U;
    while (offset + 2U <= size) {
        if (data[offset] != 0xFFU) {
            break;
        }
        while (offset + 1U < size && data[offset + 1U] == 0xFFU) {
            ++offset;
        }
        if (offset + 1U >= size) {
            break;
        }
        const std::uint8_t marker = data[offset + 1U];
        offset += 2U;
        if (marker == 0xD9U || marker == 0xDAU) {
            break;
        }
        if (marker >= 0xD0U && marker <= 0xD7U) {
            continue;
        }
        if (offset + 2U > size) {
            break;
        }
        const std::uint16_t segmentLength = readBig16(data + offset);
        if (segmentLength < 2U || segmentLength > size - offset) {
            break;
        }
        const std::uint8_t* const payload = data + offset + 2U;
        const std::size_t payloadSize = segmentLength - 2U;
        if (marker == 0xE2U && payloadSize >= 14U &&
            std::memcmp(payload, "ICC_PROFILE\0", 12) == 0) {
            *profileDeclared = true;
            const int sequence = payload[12];
            const int count = payload[13];
            if (sequence >= 1 && count >= 1) {
                declaredCount = count;
                segments[sequence].assign(payload + 14, payload + payloadSize);
            }
        }
        offset += segmentLength;
    }
    if (!*profileDeclared) {
        return false;
    }
    if (declaredCount <= 0 || static_cast<int>(segments.size()) != declaredCount) {
        setError(error, "JPEG ICC profile segments are incomplete.");
        return false;
    }
    profile->clear();
    for (int sequence = 1; sequence <= declaredCount; ++sequence) {
        const auto& segment = segments[sequence];
        if (segment.empty()) {
            setError(error, "JPEG ICC profile segment is missing.");
            return false;
        }
        if (profile->size() + segment.size() > kMaximumProfileBytes) {
            setError(error, "Embedded ICC profile exceeds the size cap.");
            return false;
        }
        profile->insert(profile->end(), segment.begin(), segment.end());
    }
    return !profile->empty();
}

// WebP: RIFF container with an ICCP chunk.
[[nodiscard]] bool extractWebpProfile(const std::uint8_t* data,
                                      const std::size_t size,
                                      std::vector<std::uint8_t>* profile,
                                      bool* profileDeclared,
                                      std::string* error) {
    if (size < 12U || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WEBP", 4) != 0) {
        return false;
    }
    std::size_t offset = 12U;
    while (offset + 8U <= size) {
        const std::uint32_t length = readLittle32(data + offset + 4U);
        const std::size_t payloadOffset = offset + 8U;
        if (length > size - payloadOffset) {
            break;
        }
        if (std::memcmp(data + offset, "ICCP", 4) == 0) {
            *profileDeclared = true;
            if (length > kMaximumProfileBytes) {
                setError(error, "Embedded ICC profile exceeds the size cap.");
                return false;
            }
            profile->assign(data + payloadOffset, data + payloadOffset + length);
            return !profile->empty();
        }
        offset = payloadOffset + length + (length & 1U);
    }
    return false;
}

[[nodiscard]] bool extractTiffProfile(const std::uint8_t* data,
                                      const std::size_t size,
                                      std::vector<std::uint8_t>* profile,
                                      bool* profileDeclared,
                                      std::string* error) {
    if (size < 8U) {
        return false;
    }
    bool littleEndian = false;
    if (data[0] == 'I' && data[1] == 'I' && data[2] == 0x2AU && data[3] == 0x00U) {
        littleEndian = true;
    } else if (data[0] == 'M' && data[1] == 'M' && data[2] == 0x00U && data[3] == 0x2AU) {
        littleEndian = false;
    } else {
        return false;
    }
    const auto read16 = [littleEndian](const std::uint8_t* pointer) {
        return littleEndian ? readLittle16(pointer) : readBig16(pointer);
    };
    const auto read32 = [littleEndian](const std::uint8_t* pointer) {
        return littleEndian ? readLittle32(pointer) : readBig32(pointer);
    };
    const std::uint32_t ifdOffset = read32(data + 4U);
    if (ifdOffset > size - 2U) {
        setError(error, "TIFF directory offset is outside the file.");
        return false;
    }
    const std::uint16_t entryCount = read16(data + ifdOffset);
    const std::size_t entriesOffset = static_cast<std::size_t>(ifdOffset) + 2U;
    if (static_cast<std::size_t>(entryCount) > (size - entriesOffset) / 12U) {
        setError(error, "TIFF directory entries exceed the file.");
        return false;
    }
    for (std::size_t index = 0; index < entryCount; ++index) {
        const std::uint8_t* const entry = data + entriesOffset + index * 12U;
        const std::uint16_t tag = read16(entry);
        if (tag != 34675U) {
            continue;
        }
        *profileDeclared = true;
        const std::uint16_t type = read16(entry + 2U);
        const std::uint32_t valueCount = read32(entry + 4U);
        if (type != 1U && type != 7U) {
            setError(error, "TIFF ICC tag has an unexpected field type.");
            return false;
        }
        if (valueCount > kMaximumProfileBytes) {
            setError(error, "Embedded ICC profile exceeds the size cap.");
            return false;
        }
        if (valueCount <= 4U) {
            profile->assign(entry + 8U, entry + 8U + valueCount);
            return !profile->empty();
        }
        const std::uint32_t valueOffset = read32(entry + 8U);
        if (valueOffset > size || valueCount > size - valueOffset) {
            setError(error, "TIFF ICC tag points outside the file.");
            return false;
        }
        profile->assign(data + valueOffset, data + valueOffset + valueCount);
        return !profile->empty();
    }
    return false;
}

} // namespace

bool extractEmbeddedIccProfile(const std::uint8_t* data,
                               const std::size_t size,
                               std::vector<std::uint8_t>* profile,
                               bool* profileDeclared,
                               std::string* error) {
    if (profile == nullptr) {
        setError(error, "No profile destination was supplied.");
        return false;
    }
    profile->clear();
    if (profileDeclared != nullptr) {
        *profileDeclared = false;
    }
    if (data == nullptr || size == 0U) {
        return false;
    }
    bool declared = false;
    const bool extracted = extractPngProfile(data, size, profile, &declared, error) ||
                           extractJpegProfile(data, size, profile, &declared, error) ||
                           extractWebpProfile(data, size, profile, &declared, error) ||
                           extractTiffProfile(data, size, profile, &declared, error);
    if (profileDeclared != nullptr) {
        *profileDeclared = declared;
    }
    return extracted;
}

} // namespace dvs::media
