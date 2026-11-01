#include "dvs/ui/ImageColorContract.h"

#include <QCryptographicHash>
#include <QStringView>

#include <cstdint>
#include <cstring>

namespace dvs::ui {
namespace {

constexpr int kIccHeaderSize = 128;
constexpr int kIccDeclaredSizeOffset = 0;
constexpr int kIccVersionOffset = 8;
constexpr int kIccClassOffset = 12;
constexpr int kIccDataSpaceOffset = 16;
constexpr int kIccPcsOffset = 20;
constexpr int kIccSignatureOffset = 36;

[[nodiscard]] std::uint32_t readBigEndian32(const QByteArray& bytes, const int offset) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2])) << 8U) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 3]));
}

// ICC header signatures are four ASCII bytes, often with a trailing space; they are kept
// verbatim so a caller can tell "RGB " apart from "Lab ".
[[nodiscard]] QString signatureText(const QByteArray& bytes, const int offset) {
    return QString::fromLatin1(bytes.constData() + offset, 4);
}

[[nodiscard]] bool knownProfileClass(const QString& value) {
    return value == QStringLiteral("mntr") || value == QStringLiteral("scnr") ||
           value == QStringLiteral("prtr") || value == QStringLiteral("spac") ||
           value == QStringLiteral("abst") || value == QStringLiteral("nmcl") ||
           value == QStringLiteral("link");
}

[[nodiscard]] bool knownDataSpace(const QString& value) {
    return value == QStringLiteral("RGB ") || value == QStringLiteral("GRAY") ||
           value == QStringLiteral("CMYK") || value == QStringLiteral("Lab ");
}

} // namespace

ImageColorInfo classifyImageColorProfile(const QByteArray& profileBytes) {
    return classifyImageColorProfile(profileBytes, ImageColorSourceLayout::Rgb);
}

ImageColorInfo classifyImageColorProfile(const QByteArray& profileBytes,
                                         const ImageColorSourceLayout layout) {
    ImageColorInfo info;
    if (profileBytes.isEmpty()) {
        info.profileStatus = ImageColorProfileStatus::None;
        return info;
    }
    // Any bytes that do not describe a structurally complete ICC header are corrupt, and the
    // status says so instead of letting a later display path treat them as absent.
    info.profileStatus = ImageColorProfileStatus::Corrupt;
    if (profileBytes.size() < kIccHeaderSize) {
        return info;
    }
    const std::uint32_t declaredSize = readBigEndian32(profileBytes, kIccDeclaredSizeOffset);
    if (declaredSize < static_cast<std::uint32_t>(kIccHeaderSize) ||
        declaredSize > static_cast<std::uint32_t>(profileBytes.size())) {
        return info;
    }
    if (std::memcmp(profileBytes.constData() + kIccSignatureOffset, "acsp", 4) != 0) {
        return info;
    }

    // From here on the header is structurally valid: hash and signatures are meaningful even
    // when the colour space or version is outside the first batch.
    info.profileSha256 = QString::fromLatin1(
        QCryptographicHash::hash(profileBytes, QCryptographicHash::Sha256).toHex());
    info.profileDataSpace = signatureText(profileBytes, kIccDataSpaceOffset);
    info.profileClass = signatureText(profileBytes, kIccClassOffset);
    info.profileVersionMajor = static_cast<std::uint8_t>(profileBytes[kIccVersionOffset]);
    info.profileStatus = ImageColorProfileStatus::EmbeddedUnsupported;

    const QString pcs = signatureText(profileBytes, kIccPcsOffset);
    const bool knownPcs = pcs == QStringLiteral("XYZ ") || pcs == QStringLiteral("Lab ");
    if (!knownProfileClass(info.profileClass) || !knownDataSpace(info.profileDataSpace) ||
        !knownPcs) {
        return info;
    }
    if (info.profileVersionMajor < 2 || info.profileVersionMajor > 4) {
        return info;
    }
    // First SDR batch: matrix/shaper RGB and gray display profiles, and only when the profile's
    // data colour space matches the decoded source layout. A gray profile on RGB data (or the
    // reverse) is an explicit fallback, not a silent conversion. CMYK/Lab source profiles also
    // remain an explicit fallback.
    const bool spaceMatchesLayout =
        (layout == ImageColorSourceLayout::Rgb &&
         info.profileDataSpace == QStringLiteral("RGB ")) ||
        (layout == ImageColorSourceLayout::Gray && info.profileDataSpace == QStringLiteral("GRAY"));
    if (spaceMatchesLayout) {
        info.profileStatus = ImageColorProfileStatus::EmbeddedSupported;
    }
    info.conversionApplied = false;
    return info;
}

const char* imageColorProfileStatusToken(const ImageColorProfileStatus status) noexcept {
    switch (status) {
    case ImageColorProfileStatus::NotProbed:
        return "not-probed";
    case ImageColorProfileStatus::None:
        return "none";
    case ImageColorProfileStatus::EmbeddedSupported:
        return "embedded-supported";
    case ImageColorProfileStatus::EmbeddedUnsupported:
        return "embedded-unsupported";
    case ImageColorProfileStatus::Corrupt:
        return "corrupt";
    }
    return "unknown";
}

const char* imagePixelValuePathToken(const ImagePixelValuePath path) noexcept {
    switch (path) {
    case ImagePixelValuePath::FileRgba8CodeValues:
        return "file-rgba8-code-values";
    case ImagePixelValuePath::DecodedDisplayRgba8:
        return "decoded-display-rgba8";
    case ImagePixelValuePath::ConvertedHighDepthRgba16:
        return "converted-high-depth-rgba16";
    case ImagePixelValuePath::DisplayDerivative:
        return "display-derivative";
    case ImagePixelValuePath::ScreenCapture:
        return "screen-capture";
    }
    return "unknown";
}

bool imagePixelPathIsFileValues(const ImagePixelValuePath path) noexcept {
    return path == ImagePixelValuePath::FileRgba8CodeValues;
}

bool imageColorProfileNeedsFallback(const ImageColorProfileStatus status) noexcept {
    return status == ImageColorProfileStatus::EmbeddedUnsupported ||
           status == ImageColorProfileStatus::Corrupt;
}

} // namespace dvs::ui
