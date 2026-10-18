#pragma once

#include <QByteArray>
#include <QString>

namespace dvs::ui {

// COLOR1 data contract: what a pixel number actually is, and what the file declared about
// colour. Classification itself is descriptive only. COLOR4 adds a display-only conversion in
// the review controller; analysis buffers and statistics still keep the values described here.
//
// The same pixel can be described at four different points, and they are not interchangeable:
//   file code values        -> decode conversion -> analysis buffer -> display derivative
// A probe or statistic reads the analysis buffer (RGBA8 display values, plus an optional
// decoded RGBA64 sidecar for high-bit-depth sources). The screen shows a presentation
// derivative of it. Only a source that is already 8-bit RGBA with no conversion keeps its
// file code values in the analysis buffer; anything else is decoder-converted.
enum class ImagePixelValuePath : int {
    // Decoded source was plain 8-bit RGBA; the analysis buffer holds the file's own codes.
    FileRgba8CodeValues = 0,
    // The decoder/swscale converted the source (bit depth, channels or pixel format) into
    // the RGBA8 analysis buffer. The numbers are display values, not file plane codes.
    DecodedDisplayRgba8 = 1,
    // The decoded RGBA64 sidecar of a high-bit-depth source. Still decoder-converted values,
    // never the file's original 16-bit plane; used for the explicit "converted 16-bit" stats.
    ConvertedHighDepthRgba16 = 2,
    // Channel view or difference image rendered for display. Presentation only; never a
    // source for statistics, editing or an "original pixel" claim.
    DisplayDerivative = 3,
    // A screenshot of the rendered window. It records the display result, not source pixels.
    ScreenCapture = 4,
};

// Structural state of the file's embedded ICC profile, judged without a colour engine.
enum class ImageColorProfileStatus : int {
    // The decoder path did not expose the profile bytes (for example a Qt fallback load).
    NotProbed = 0,
    // The file carries no embedded profile.
    None = 1,
    // Structurally a usable ICC v2/v4 RGB or gray profile for the first SDR batch.
    // This only means the bytes are recognised; it does not mean the display is colour-managed.
    EmbeddedSupported = 2,
    // A well-formed ICC header whose data colour space, profile class or version is outside
    // the first batch. Callers must show a fallback instead of claiming colour handling.
    EmbeddedUnsupported = 3,
    // Profile bytes are present but fail the structural header checks.
    Corrupt = 4,
};

struct ImageColorInfo final {
    ImageColorProfileStatus profileStatus = ImageColorProfileStatus::NotProbed;
    // Hex SHA-256 of the exact embedded profile bytes; empty when no profile or not probed.
    QString profileSha256;
    // ICC header signatures kept verbatim: data colour space ("RGB ", "GRAY", "CMYK", "Lab ")
    // and profile class ("mntr", "scnr", "prtr", …).
    QString profileDataSpace;
    QString profileClass;
    // Major version byte of the ICC header (2, 4, …); 0 when no profile / not probed.
    int profileVersionMajor = 0;
    // FFmpeg AVColorPrimaries / AVColorTransferCharacteristic / AVColorSpace declarations of
    // the decoded frame, or -1 when the file did not declare them. Declarations only.
    int declaredPrimaries = -1;
    int declaredTransfer = -1;
    int declaredColorSpace = -1;
    // True only once a colour-managed conversion produced the display values. Classification
    // always leaves it false; the review controller owns the COLOR4 display derivative.
    bool conversionApplied = false;
};

// Shape of the decoded source the profile is attached to. A structurally valid gray profile
// on an RGB source is not applicable, and vice versa; both must fall back explicitly instead
// of being reported as a usable source profile.
enum class ImageColorSourceLayout : int {
    Rgb = 0,
    Gray = 1,
};

// Classifies raw ICC bytes without applying them. Empty input is None; anything that fails
// the header checks is Corrupt; a well-formed header outside the first batch (or attached to
// the wrong source layout) is EmbeddedUnsupported. SHA-256 is taken over the exact bytes
// supplied. The single-argument form keeps the historical RGB-source default.
[[nodiscard]] ImageColorInfo classifyImageColorProfile(const QByteArray& profileBytes);
[[nodiscard]] ImageColorInfo classifyImageColorProfile(const QByteArray& profileBytes,
                                                       ImageColorSourceLayout layout);

// Stable ASCII tokens for logs and tests. Localized UI text belongs in QML.
[[nodiscard]] const char* imageColorProfileStatusToken(ImageColorProfileStatus status) noexcept;
[[nodiscard]] const char* imagePixelValuePathToken(ImagePixelValuePath path) noexcept;

// True only for the path that really carries the file's own RGBA8 code values.
[[nodiscard]] bool imagePixelPathIsFileValues(ImagePixelValuePath path) noexcept;

// True when the profile state requires a visible fallback (unsupported or corrupt), so the UI
// can style it differently from the neutral "declared but not applied" state.
[[nodiscard]] bool imageColorProfileNeedsFallback(ImageColorProfileStatus status) noexcept;

} // namespace dvs::ui
