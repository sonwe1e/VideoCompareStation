#pragma once

#include "dvs/ui/ImagePairLoader.h"

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QObject>
#include <QUrl>
#include <QVariantMap>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace dvs::ui {

// CPU-side still-image workspace: load, zoom/pan, channel sampling, and two-image diff.
// Kept off the video FrameSet path so image tools stay independent of decode cadence.
class ImageReviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasPrimary READ hasPrimary NOTIFY stateChanged)
    Q_PROPERTY(bool hasSecondary READ hasSecondary NOTIFY stateChanged)
    Q_PROPERTY(bool hasPair READ hasPair NOTIFY stateChanged)
    Q_PROPERTY(int primaryWidth READ primaryWidth NOTIFY stateChanged)
    Q_PROPERTY(int primaryHeight READ primaryHeight NOTIFY stateChanged)
    Q_PROPERTY(int secondaryWidth READ secondaryWidth NOTIFY stateChanged)
    Q_PROPERTY(int secondaryHeight READ secondaryHeight NOTIFY stateChanged)
    Q_PROPERTY(QString primaryPath READ primaryPath NOTIFY stateChanged)
    Q_PROPERTY(QString secondaryPath READ secondaryPath NOTIFY stateChanged)
    Q_PROPERTY(int compareMode READ compareMode WRITE setCompareMode NOTIFY stateChanged)
    Q_PROPERTY(int viewMode READ viewMode WRITE setViewMode NOTIFY stateChanged)
    Q_PROPERTY(qreal wipePosition READ wipePosition WRITE setWipePosition NOTIFY viewChanged)
    // E1: wipe split axis for the image workspace. 0 = Vertical (left/right), 1 = Horizontal
    // (top/bottom). View-only state like wipePosition: never persisted, never touches the
    // decoded buffers, and the clip paths for both images follow the same axis.
    Q_PROPERTY(int wipeOrientation READ wipeOrientation WRITE setWipeOrientation NOTIFY viewChanged)
    // Fade blend position for the two-image Fade mode: 0 shows A, 1 shows B. View-only state,
    // never touches the decoded buffers or the difference pipeline.
    Q_PROPERTY(qreal fadePosition READ fadePosition WRITE setFadePosition NOTIFY viewChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY viewChanged)
    Q_PROPERTY(qreal panX READ panX NOTIFY viewChanged)
    Q_PROPERTY(qreal panY READ panY NOTIFY viewChanged)
    Q_PROPERTY(QVariantMap cursorPixel READ cursorPixel NOTIFY cursorPixelChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(int contentGeneration READ contentGeneration NOTIFY stateChanged)
    Q_PROPERTY(int maxAbsDifference READ maxAbsDifference NOTIFY stateChanged)
    Q_PROPERTY(double meanAbsDifference READ meanAbsDifference NOTIFY stateChanged)
    Q_PROPERTY(int peakAlphaDifference READ peakAlphaDifference NOTIFY stateChanged)
    Q_PROPERTY(double meanAlphaDifference READ meanAlphaDifference NOTIFY stateChanged)
    Q_PROPERTY(qint64 alphaChangedPixels READ alphaChangedPixels NOTIFY stateChanged)
    Q_PROPERTY(int committedPairId READ committedPairId NOTIFY stateChanged)
    Q_PROPERTY(bool hasDiffResult READ hasDiffResult NOTIFY stateChanged)
    Q_PROPERTY(bool diffResampled READ diffResampled NOTIFY stateChanged)
    Q_PROPERTY(bool alphaDifferenceOnly READ alphaDifferenceOnly NOTIFY stateChanged)
    Q_PROPERTY(bool diffHasAlpha READ diffHasAlpha NOTIFY stateChanged)
    Q_PROPERTY(
        bool resampleAllowed READ resampleAllowed WRITE setResampleAllowed NOTIFY stateChanged)
    Q_PROPERTY(int diffGain READ diffGain WRITE setDiffGain NOTIFY stateChanged)
    Q_PROPERTY(QString diffScopeText READ diffScopeText NOTIFY stateChanged)
    Q_PROPERTY(bool openPending READ openPending NOTIFY stateChanged)
    Q_PROPERTY(bool diffPending READ diffPending NOTIFY stateChanged)
    // Converted 16-bit statistics: the numbers that stay meaningful when the RGBA8 display
    // buffer cannot show the difference.
    Q_PROPERTY(bool nativeStatsAvailable READ nativeStatsAvailable NOTIFY stateChanged)
    Q_PROPERTY(int nativeMaxAbsDifference READ nativeMaxAbsDifference NOTIFY stateChanged)
    Q_PROPERTY(double nativeMeanAbsDifference READ nativeMeanAbsDifference NOTIFY stateChanged)
    Q_PROPERTY(int nativePeakAlphaDifference READ nativePeakAlphaDifference NOTIFY stateChanged)
    Q_PROPERTY(qint64 nativeChangedPixels READ nativeChangedPixels NOTIFY stateChanged)
    Q_PROPERTY(qint64 nativeBeyondDisplayPixels READ nativeBeyondDisplayPixels NOTIFY stateChanged)
    Q_PROPERTY(
        bool displayEqualButNativeDifferent READ displayEqualButNativeDifferent NOTIFY stateChanged)
    Q_PROPERTY(QString nativeStatsText READ nativeStatsText NOTIFY stateChanged)
    Q_PROPERTY(qint64 workingSetBudgetBytes READ workingSetBudgetBytes WRITE
                   setWorkingSetBudgetBytes NOTIFY stateChanged)
    // Source provenance of each side: bit depth, decoded format and alpha presence, so the
    // UI can distinguish original code values from display-converted RGBA8 samples.
    Q_PROPERTY(int primaryBitDepth READ primaryBitDepth NOTIFY stateChanged)
    Q_PROPERTY(int primaryChannels READ primaryChannels NOTIFY stateChanged)
    Q_PROPERTY(bool primaryHasAlpha READ primaryHasAlpha NOTIFY stateChanged)
    Q_PROPERTY(QString primarySourceFormat READ primarySourceFormat NOTIFY stateChanged)
    Q_PROPERTY(bool primaryDisplayConverted READ primaryDisplayConverted NOTIFY stateChanged)
    Q_PROPERTY(int secondaryBitDepth READ secondaryBitDepth NOTIFY stateChanged)
    Q_PROPERTY(int secondaryChannels READ secondaryChannels NOTIFY stateChanged)
    Q_PROPERTY(bool secondaryHasAlpha READ secondaryHasAlpha NOTIFY stateChanged)
    Q_PROPERTY(QString secondarySourceFormat READ secondarySourceFormat NOTIFY stateChanged)
    Q_PROPERTY(bool secondaryDisplayConverted READ secondaryDisplayConverted NOTIFY stateChanged)

    // ---- COLOR1 colour/data contract, COLOR3 status source --------------------------------
    // Per side: the embedded ICC profile state (classified, never applied here), the ICC
    // signatures, and the colour declarations the file carried. The status text is the one
    // source the badge, its tooltip and any exported caption must share.
    Q_PROPERTY(int primaryColorProfileStatus READ primaryColorProfileStatus NOTIFY stateChanged)
    Q_PROPERTY(QString primaryColorProfileSha256 READ primaryColorProfileSha256 NOTIFY stateChanged)
    Q_PROPERTY(
        QString primaryColorProfileDataSpace READ primaryColorProfileDataSpace NOTIFY stateChanged)
    Q_PROPERTY(QString primaryColorProfileClass READ primaryColorProfileClass NOTIFY stateChanged)
    Q_PROPERTY(int primaryColorProfileVersionMajor READ primaryColorProfileVersionMajor NOTIFY
                   stateChanged)
    Q_PROPERTY(int primaryDeclaredPrimaries READ primaryDeclaredPrimaries NOTIFY stateChanged)
    Q_PROPERTY(int primaryDeclaredTransfer READ primaryDeclaredTransfer NOTIFY stateChanged)
    Q_PROPERTY(int primaryDeclaredColorSpace READ primaryDeclaredColorSpace NOTIFY stateChanged)
    Q_PROPERTY(
        bool primaryColorConversionApplied READ primaryColorConversionApplied NOTIFY stateChanged)
    Q_PROPERTY(QString primaryColorStatusText READ primaryColorStatusText NOTIFY stateChanged)
    // Compact form for the always-visible A/B status badges; the full sentence above is the
    // tooltip and the exported explanation. Both come from the same state switch.
    Q_PROPERTY(
        QString primaryColorStateCompactText READ primaryColorStateCompactText NOTIFY stateChanged)
    // Unsupported or corrupt profile: the UI must style this as a fallback, not as a neutral
    // "profile declared but not applied" note.
    Q_PROPERTY(bool primaryColorProfileNeedsFallback READ primaryColorProfileNeedsFallback NOTIFY
                   stateChanged)
    Q_PROPERTY(int secondaryColorProfileStatus READ secondaryColorProfileStatus NOTIFY stateChanged)
    Q_PROPERTY(
        QString secondaryColorProfileSha256 READ secondaryColorProfileSha256 NOTIFY stateChanged)
    Q_PROPERTY(QString secondaryColorProfileDataSpace READ secondaryColorProfileDataSpace NOTIFY
                   stateChanged)
    Q_PROPERTY(
        QString secondaryColorProfileClass READ secondaryColorProfileClass NOTIFY stateChanged)
    Q_PROPERTY(int secondaryColorProfileVersionMajor READ secondaryColorProfileVersionMajor NOTIFY
                   stateChanged)
    Q_PROPERTY(int secondaryDeclaredPrimaries READ secondaryDeclaredPrimaries NOTIFY stateChanged)
    Q_PROPERTY(int secondaryDeclaredTransfer READ secondaryDeclaredTransfer NOTIFY stateChanged)
    Q_PROPERTY(int secondaryDeclaredColorSpace READ secondaryDeclaredColorSpace NOTIFY stateChanged)
    Q_PROPERTY(bool secondaryColorConversionApplied READ secondaryColorConversionApplied NOTIFY
                   stateChanged)
    Q_PROPERTY(QString secondaryColorStatusText READ secondaryColorStatusText NOTIFY stateChanged)
    Q_PROPERTY(QString secondaryColorStateCompactText READ secondaryColorStateCompactText NOTIFY
                   stateChanged)
    Q_PROPERTY(bool secondaryColorProfileNeedsFallback READ secondaryColorProfileNeedsFallback
                   NOTIFY stateChanged)
    // Which point of the pixel path the probe/difference statistics read for each side. The
    // UI uses this instead of inferring "original" from the format string.
    Q_PROPERTY(int primaryAnalysisValuePath READ primaryAnalysisValuePath NOTIFY stateChanged)
    Q_PROPERTY(int secondaryAnalysisValuePath READ secondaryAnalysisValuePath NOTIFY stateChanged)
    Q_PROPERTY(bool primaryNativeValuePathAvailable READ primaryNativeValuePathAvailable NOTIFY
                   stateChanged)
    Q_PROPERTY(bool secondaryNativeValuePathAvailable READ secondaryNativeValuePathAvailable NOTIFY
                   stateChanged)
    // Windows/display output verification is a COLOR5 claim. Until then no screen result may
    // be presented as a verified colour-managed display.
    Q_PROPERTY(bool displayColorEnvironmentVerified READ displayColorEnvironmentVerified NOTIFY
                   stateChanged)

    // ---- COLOR4 display colour conversion -----------------------------------------------
    // Applies the parsed embedded SDR profile to the display derivative only. Turning it off
    // shows the decoded values unchanged; neither state touches analysis buffers, statistics,
    // probe samples or exported source values. The status text and these flags are the one
    // source of truth for what the screen currently shows.
    Q_PROPERTY(bool colorDisplayEnabled READ colorDisplayEnabled WRITE setColorDisplayEnabled NOTIFY
                   colorDisplayChanged)
    Q_PROPERTY(bool primaryColorDisplayApplied READ primaryColorDisplayApplied NOTIFY stateChanged)
    Q_PROPERTY(
        bool secondaryColorDisplayApplied READ secondaryColorDisplayApplied NOTIFY stateChanged)

    // ---- Alpha threshold observation filter --------------------------------------------
    // A view-only filter over the "RGB ignoring alpha" observation. It never reaches the
    // committed buffers, the difference pipeline, the statistics, the edit working copy or
    // a saved copy.
    Q_PROPERTY(int numericFormat READ numericFormat WRITE setNumericFormat NOTIFY stateChanged)
    Q_PROPERTY(bool alphaFilterEnabled READ alphaFilterEnabled WRITE setAlphaFilterEnabled NOTIFY
                   stateChanged)
    Q_PROPERTY(int alphaThreshold READ alphaThreshold WRITE setAlphaThreshold NOTIFY stateChanged)
    // True only while the filter can actually change what is on screen (the RGB-ignoring
    // observation is active). The workspace uses this to say "not applied" instead of
    // silently filtering, or silently not filtering, in the other modes.
    Q_PROPERTY(bool alphaFilterActive READ alphaFilterActive NOTIFY stateChanged)

public:
    // Process-wide still-image decoder. Qt's imageformat plugins may omit PNG/JPEG on
    // minimal deploys; the app composition root injects an FFmpeg-backed loader that
    // consumes raw file bytes (Unicode-path safe). The info out-param receives the source
    // provenance; leave it null to discard.
    using StillImageLoader = std::function<bool(const QByteArray& fileBytes,
                                                QImage* image,
                                                QImage* nativeImage,
                                                StillImageSourceInfo* info,
                                                std::string* error)>;
    static void setProcessStillImageLoader(StillImageLoader loader);
    // Optional header-only dimensions probe. Returning false means "unknown", not failure;
    // the loader still decodes and checks the decoded size afterwards.
    using StillImageProbe = std::function<bool(const QByteArray& fileBytes, QSize* size)>;
    static void setProcessStillImageProbe(StillImageProbe probe);
    // Optional container-level ICC extraction, used when the decoder path does not expose the
    // profile bytes. In production it is backed by media::extractEmbeddedIccProfile(), which
    // is what keeps PNG colour state alive while the Qt fallback decodes PNG.
    using StillImageColorProbe = ImagePairLoader::ImageColorProbe;
    static void setProcessStillImageColorProbe(StillImageColorProbe probe);

    enum CompareMode : int {
        PrimaryOnly = 0,
        SideBySide = 1,
        AbsDifference = 2,
        SignedDifference = 3,
        Highlight = 4,
        Wipe = 5,
        AlphaDifference = 6,
        Fade = 7,
    };
    Q_ENUM(CompareMode)

    // Channel observation of the display buffer. The decoded image is always RGBA8; these
    // views isolate one aspect of it for transparency inspection.
    enum ImageViewMode : int {
        RgbaView = 0,
        AlphaGrayView = 1,
        RgbOpaqueView = 2,
    };
    Q_ENUM(ImageViewMode)

    // Numeric readout format. Display/copy formatting only: it never touches the decoded
    // buffers, the sampling values, the difference field or any statistic. Hex stays a
    // hexadecimal expression in both forms.
    enum NumericFormat : int {
        IntegerCodeValues = 0,
        NormalizedFloat = 1,
    };
    Q_ENUM(NumericFormat)

    // The exact clipboard payload for one frozen probe sample; an empty text from
    // probeColorText means "nothing to copy".
    enum ProbeColorFormat : int {
        ProbeRgb = 0,
        ProbeRgba = 1,
        ProbeHex = 2,
    };
    Q_ENUM(ProbeColorFormat)

    enum ImageSlot : int {
        PrimarySlot = 0,
        SecondarySlot = 1,
        DisplayPrimarySlot = 2,
        DisplaySecondarySlot = 3,
        DisplayDiffSlot = 4,
    };
    Q_ENUM(ImageSlot)

    explicit ImageReviewController(QObject* parent = nullptr);
    ~ImageReviewController() override;

    [[nodiscard]] bool hasPrimary() const noexcept;
    [[nodiscard]] bool hasSecondary() const noexcept;
    [[nodiscard]] bool hasPair() const noexcept;
    [[nodiscard]] int primaryWidth() const noexcept;
    [[nodiscard]] int primaryHeight() const noexcept;
    [[nodiscard]] int secondaryWidth() const noexcept;
    [[nodiscard]] int secondaryHeight() const noexcept;
    [[nodiscard]] QString primaryPath() const;
    [[nodiscard]] QString secondaryPath() const;
    [[nodiscard]] int compareMode() const noexcept;
    void setCompareMode(int mode);
    [[nodiscard]] int viewMode() const noexcept;
    void setViewMode(int mode);
    [[nodiscard]] qreal wipePosition() const noexcept;
    void setWipePosition(qreal position);
    [[nodiscard]] int wipeOrientation() const noexcept;
    void setWipeOrientation(int orientation);
    [[nodiscard]] qreal fadePosition() const noexcept;
    void setFadePosition(qreal position);
    [[nodiscard]] qreal zoom() const noexcept;
    [[nodiscard]] qreal panX() const noexcept;
    [[nodiscard]] qreal panY() const noexcept;
    [[nodiscard]] QVariantMap cursorPixel() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] int contentGeneration() const noexcept;
    [[nodiscard]] int maxAbsDifference() const noexcept;
    [[nodiscard]] double meanAbsDifference() const noexcept;
    [[nodiscard]] int peakAlphaDifference() const noexcept;
    [[nodiscard]] double meanAlphaDifference() const noexcept;
    [[nodiscard]] qint64 alphaChangedPixels() const noexcept;
    [[nodiscard]] int committedPairId() const noexcept;
    [[nodiscard]] bool hasDiffResult() const noexcept;
    [[nodiscard]] bool diffResampled() const noexcept;
    [[nodiscard]] bool alphaDifferenceOnly() const noexcept;
    [[nodiscard]] bool diffHasAlpha() const noexcept;
    [[nodiscard]] bool resampleAllowed() const noexcept;
    void setResampleAllowed(bool allowed);
    // Display-only amplification of the rendered difference image. The statistics stay raw
    // 8-bit deltas, so changing the gain never changes a number and different candidates stay
    // comparable while they share one gain.
    [[nodiscard]] int diffGain() const noexcept;
    void setDiffGain(int gain);
    [[nodiscard]] QString diffScopeText() const;
    [[nodiscard]] bool openPending() const noexcept;
    [[nodiscard]] bool diffPending() const noexcept;

    // Converted RGBA64 (16-bit) statistics of the current pair. Available only when both sides
    // carry an original-depth sidecar and no resampling occurred. These are decoder-converted
    // 16-bit code values, never raw file planes.
    [[nodiscard]] bool nativeStatsAvailable() const noexcept;
    [[nodiscard]] int nativeMaxAbsDifference() const noexcept;
    [[nodiscard]] double nativeMeanAbsDifference() const noexcept;
    [[nodiscard]] int nativePeakAlphaDifference() const noexcept;
    [[nodiscard]] qint64 nativeChangedPixels() const noexcept;
    [[nodiscard]] qint64 nativeBeyondDisplayPixels() const noexcept;
    [[nodiscard]] bool displayEqualButNativeDifferent() const noexcept;
    // One sentence naming which path the readouts come from and whether the display buffer hid a
    // difference. Empty when no high-depth statistics exist for the current pair.
    [[nodiscard]] QString nativeStatsText() const;

    // Whole-working-set ceiling for one compared pair (display buffers, RGBA64 sidecars, the
    // per-pair analysis field and the derived difference image). Distinct from the decoded-image
    // cache budget: this is the peak residency the diff pipeline needs, not a cache size.
    [[nodiscard]] qint64 workingSetBudgetBytes() const noexcept;
    void setWorkingSetBudgetBytes(qint64 bytes);

    [[nodiscard]] int primaryBitDepth() const noexcept;
    [[nodiscard]] int primaryChannels() const noexcept;
    [[nodiscard]] bool primaryHasAlpha() const noexcept;
    [[nodiscard]] QString primarySourceFormat() const;
    [[nodiscard]] bool primaryDisplayConverted() const noexcept;
    [[nodiscard]] int secondaryBitDepth() const noexcept;
    [[nodiscard]] int secondaryChannels() const noexcept;
    [[nodiscard]] bool secondaryHasAlpha() const noexcept;
    [[nodiscard]] QString secondarySourceFormat() const;
    [[nodiscard]] bool secondaryDisplayConverted() const noexcept;

    // ---- COLOR1 colour/data contract, COLOR3 status source ------------------------------
    [[nodiscard]] int primaryColorProfileStatus() const noexcept;
    [[nodiscard]] QString primaryColorProfileSha256() const;
    [[nodiscard]] QString primaryColorProfileDataSpace() const;
    [[nodiscard]] QString primaryColorProfileClass() const;
    [[nodiscard]] int primaryColorProfileVersionMajor() const noexcept;
    [[nodiscard]] int primaryDeclaredPrimaries() const noexcept;
    [[nodiscard]] int primaryDeclaredTransfer() const noexcept;
    [[nodiscard]] int primaryDeclaredColorSpace() const noexcept;
    [[nodiscard]] bool primaryColorConversionApplied() const noexcept;
    // The single localized colour-state sentence for one side. The badge, its tooltip, the
    // copy/export captions and any status line must read this instead of re-deriving state.
    [[nodiscard]] QString primaryColorStatusText() const;
    [[nodiscard]] QString primaryColorStateCompactText() const;
    [[nodiscard]] bool primaryColorProfileNeedsFallback() const noexcept;
    [[nodiscard]] int secondaryColorProfileStatus() const noexcept;
    [[nodiscard]] QString secondaryColorProfileSha256() const;
    [[nodiscard]] QString secondaryColorProfileDataSpace() const;
    [[nodiscard]] QString secondaryColorProfileClass() const;
    [[nodiscard]] int secondaryColorProfileVersionMajor() const noexcept;
    [[nodiscard]] int secondaryDeclaredPrimaries() const noexcept;
    [[nodiscard]] int secondaryDeclaredTransfer() const noexcept;
    [[nodiscard]] int secondaryDeclaredColorSpace() const noexcept;
    [[nodiscard]] bool secondaryColorConversionApplied() const noexcept;
    [[nodiscard]] QString secondaryColorStatusText() const;
    [[nodiscard]] QString secondaryColorStateCompactText() const;
    [[nodiscard]] bool secondaryColorProfileNeedsFallback() const noexcept;
    [[nodiscard]] int primaryAnalysisValuePath() const noexcept;
    [[nodiscard]] int secondaryAnalysisValuePath() const noexcept;
    [[nodiscard]] bool primaryNativeValuePathAvailable() const noexcept;
    [[nodiscard]] bool secondaryNativeValuePathAvailable() const noexcept;
    [[nodiscard]] bool displayColorEnvironmentVerified() const noexcept;

    // ---- COLOR4 display colour conversion ----------------------------------------------
    [[nodiscard]] bool colorDisplayEnabled() const noexcept;
    void setColorDisplayEnabled(bool enabled);
    // True when the current side's display derivative really passed through the file's
    // profile. False for no profile, an unsupported/corrupt profile, or the switch being off.
    [[nodiscard]] bool primaryColorDisplayApplied() const noexcept;
    [[nodiscard]] bool secondaryColorDisplayApplied() const noexcept;

    // ---- Single-side close ---------------------------------------------------------------
    // Closes exactly one side of the workspace. Neither is a disguised closeAll: with a
    // pair committed the surviving side is kept (closing A promotes B into the internal
    // primary slot so the file identity, the decoded content and the source provenance
    // stay correct), and only when the named side is the last image does the workspace
    // reach the empty state.
    Q_INVOKABLE bool closePrimary();
    Q_INVOKABLE bool closeSecondary();

    // ---- Numeric readout format ----------------------------------------------------------
    [[nodiscard]] int numericFormat() const noexcept;
    void setNumericFormat(int format);
    // Channel text for the readouts and the copied colour. Integer mode keeps the code
    // value; float mode normalizes it against the buffer it came from (8-bit display
    // buffer: channel/255; converted 16-bit sidecar: channel16/65535).
    [[nodiscard]] Q_INVOKABLE QString channelText(int channel8) const;
    [[nodiscard]] Q_INVOKABLE QString channelText16(int channel16) const;
    // Signed delta text. The sign is part of the meaning (Δ = B − A), so a negative delta
    // is never clipped to zero and a zero delta never renders as "-0.000000".
    [[nodiscard]] Q_INVOKABLE QString deltaText(int low8, int high8) const;
    [[nodiscard]] Q_INVOKABLE QString deltaText16(int low16, int high16) const;
    [[nodiscard]] Q_INVOKABLE static QString hexText(int r, int g, int b, int a);
    [[nodiscard]] Q_INVOKABLE QString rgbText(int r, int g, int b) const;
    [[nodiscard]] Q_INVOKABLE QString rgbaText(int r, int g, int b, int a) const;

    // ---- Alpha threshold observation filter ----------------------------------------------
    [[nodiscard]] bool alphaFilterEnabled() const noexcept;
    void setAlphaFilterEnabled(bool enabled);
    // 0..255 equivalent scale. The comparison is a strict "A > T".
    [[nodiscard]] int alphaThreshold() const noexcept;
    void setAlphaThreshold(int threshold);
    [[nodiscard]] bool alphaFilterActive() const noexcept;
    // Whether one pixel's alpha passes the active filter. A high-bit-depth side compares
    // its un-quantized sidecar alpha (T maps to T/255, so the 16-bit bound is
    // T * 65535 / 255 evaluated exactly in integers); an 8-bit buffer compares its own
    // alpha. Never rounds the source down to 8 bits first.
    [[nodiscard]] bool alphaPassesFilter(bool primarySide, int alpha8, quint16 alpha16) const;
    // Convenience form for the probe readouts: decides from the pixel's own position, so a
    // high-depth side is judged by its sidecar alpha rather than by the 8-bit display
    // value the probe reports.
    [[nodiscard]] bool alphaHiddenAt(bool primarySide, int x, int y, int alpha8) const;

    // ---- Frozen probe sample --------------------------------------------------------------
    // The last valid sample of the current material generation. The pointer may leave the
    // pixel (and the canvas) without losing it, so the copy button never has to guess a
    // position or read the screen colour under itself.
    Q_PROPERTY(QVariantMap frozenProbeSample READ frozenProbeSample NOTIFY frozenProbeSampleChanged)
    [[nodiscard]] QVariantMap frozenProbeSample() const;
    Q_INVOKABLE bool captureProbeSample(int imageSlot, qreal imageX, qreal imageY);
    Q_INVOKABLE void clearFrozenProbeSample();
    // The exact clipboard payload for one frozen sample. Format is one of
    // ProbeColorFormat (declared above); an empty string means "nothing to copy".
    [[nodiscard]] Q_INVOKABLE QString probeColorText(int format) const;

    Q_INVOKABLE bool openPrimary(const QUrl& url);
    Q_INVOKABLE bool openSecondary(const QUrl& url);
    // Session-local A/B orientation flip. Swaps both sides (buffers, paths, provenance)
    // without touching committedPairId, zoom/pan, or the folder row. Direction-sensitive
    // views (signed difference, wipe/fade labels) follow the swapped A/B roles.
    Q_INVOKABLE bool swapSides();
    // Opens both sides as one transaction: every validation runs before any state changes,
    // and on success the pair, paths and committedPairId switch in a single generation bump.
    // On failure the previous pair (or the explicit empty state) is fully retained and the
    // failure source is kept in errorText.
    Q_INVOKABLE bool
    openPairAtomically(const QUrl& primary, const QUrl& secondary, int pairId = -1);
    // Image-injection form of openPairAtomically (tests and non-file sources). The optional
    // info values carry source provenance; the five-argument overload keeps the historical
    // symbol for callers that do not supply provenance.
    bool openPairImages(QImage primary,
                        QString primaryLabel,
                        QImage secondary,
                        QString secondaryLabel,
                        int pairId = -1);
    bool openPairImages(QImage primary,
                        QString primaryLabel,
                        QImage secondary,
                        QString secondaryLabel,
                        int pairId,
                        StillImageSourceInfo primaryInfo,
                        StillImageSourceInfo secondaryInfo);
    Q_INVOKABLE void closeAll();
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void zoomBy(qreal factor, qreal anchorNormalizedX, qreal anchorNormalizedY);
    Q_INVOKABLE void
    zoomToRect(qreal normalizedX0, qreal normalizedY0, qreal normalizedX1, qreal normalizedY1);
    Q_INVOKABLE void panBy(qreal deltaNormalizedX, qreal deltaNormalizedY);
    Q_INVOKABLE void setZoom(qreal value);
    // Restores a recorded observation position (zoom + normalized pan) in one step. Out of
    // range or non-finite values are clamped so restore cannot move the view to an invalid
    // state.
    Q_INVOKABLE void restoreObservationPosition(qreal zoom, qreal panX, qreal panY);
    Q_INVOKABLE QVariantMap samplePixel(int imageSlot, qreal imageX, qreal imageY) const;
    // Same-coordinate pair readout support: samples the committed decode buffer of one side
    // WITHOUT the active channel observation view (the derived Alpha-gray/RGB-opaque view is
    // a presentation artifact and must never read as the side's original RGBA). Accepts the
    // primary slots (0/2) and secondary slots (1/3) only; the difference image is not
    // reachable here so a pair readout can never mix difference pixels into A/B originals.
    // Reports the source label "original" and, when the side carries a high-bit-depth
    // sidecar, its original-depth code values as well.
    Q_INVOKABLE QVariantMap samplePixelOriginal(int imageSlot, qreal imageX, qreal imageY) const;
    Q_INVOKABLE void updateCursorPixel(int imageSlot, qreal imageX, qreal imageY);
    Q_INVOKABLE void clearCursorPixel();
    Q_INVOKABLE QString imageUrl(int imageSlot) const;

    // T4 asynchronous entries. Each returns a positive request id when accepted; the final
    // commit or error is delivered through openFinished on the GUI thread. A newer request
    // invalidates older candidates, so a late N cannot replace N+2.
    // pairId carries the caller's row identity (folder single-side rows) so the
    // completion can advance that list selection; loose opens keep the default -1.
    Q_INVOKABLE int requestOpenPrimary(const QUrl& url, int pairId = -1);
    Q_INVOKABLE int requestOpenSecondary(const QUrl& url);
    Q_INVOKABLE int requestOpenPair(const QUrl& primary, const QUrl& secondary, int pairId = -1);
    // Single-side slot replace: only the named side is swapped in. The other side,
    // committedPairId and the observation position stay put (T1). Failure keeps the
    // previous side fully intact. requestOpenPrimary remains "open a new first image"
    // and still clears B; use these when the user means "换 A / 换 B".
    Q_INVOKABLE int requestReplacePrimary(const QUrl& url);
    Q_INVOKABLE int requestReplaceSecondary(const QUrl& url);
    Q_INVOKABLE void cancelPendingOpen();
    Q_INVOKABLE void cancelOpenRequest(int requestId);
    // One bounded neighbour read. It only warms the decoded-image cache; it never changes
    // the visible pair or committed identity.
    Q_INVOKABLE void prefetchPair(const QUrl& primary, const QUrl& secondary);
    Q_INVOKABLE QVariantMap asyncStats() const;
    Q_INVOKABLE void clearAsyncCaches();

    // Direct image injection (tests and non-file sources). The optional info carries source
    // provenance so the UI can label display-converted samples honestly. The two-argument
    // overloads are real functions defined in the library (not defaulted parameters), so
    // callers compiled against the historical two-parameter symbol keep linking.
    bool openPrimaryImage(QImage image, QString pathLabel);
    bool openPrimaryImage(QImage image, QString pathLabel, StillImageSourceInfo info);
    bool openSecondaryImage(QImage image, QString pathLabel);
    bool openSecondaryImage(QImage image, QString pathLabel, StillImageSourceInfo info);

    // Test seam: delivers a loader result to the completion handler without going through the
    // loader. Used to prove a stale request id cannot commit over a newer pending request;
    // production code never calls it. The delivered result still passes every identity check
    // the asynchronous path uses.
    void deliverLoadResultForTesting(ImagePairLoader::Result result);

    // Used by ReviewImageProvider. Thread: GUI only (still images are CPU-resident).
    [[nodiscard]] QImage imageForSlot(int imageSlot) const;
    // Committed decoded buffer without the channel observation view. Editing and other
    // "original pixels" consumers must use this: a derived Alpha-gray/RGB-opaque view is
    // a presentation artifact and must never be edited or re-saved as the source.
    [[nodiscard]] QImage rawImageForSlot(int imageSlot) const;

signals:
    void stateChanged();
    void viewChanged();
    void cursorPixelChanged();
    void frozenProbeSampleChanged();
    void colorDisplayChanged();
    // T4 completion contract: exactly one terminal per accepted asynchronous request.
    void openFinished(int requestId, int pairId, bool success, QString error);

private:
    struct AsyncState;

    void setError(QString text);
    void bumpGeneration();
    [[nodiscard]] static bool loadChecked(const QUrl& url,
                                          QImage* image,
                                          QImage* nativeImage,
                                          StillImageSourceInfo* info,
                                          QString* error);
    [[nodiscard]] QImage displayImage(int slot) const;
    // COLOR4: converts a decoded buffer to sRGB for presentation when the switch is on, the
    // profile state is supported and the side carries a parsed profile. Never used for
    // analysis, statistics or probe samples.
    [[nodiscard]] QImage displayColorImage(const QImage& source,
                                           const StillImageSourceInfo& info) const;
    // Derived channel view of a decoded buffer for the active observation mode. Result is
    // cached across calls and invalidated when the generation, the view mode or the alpha
    // filter changes. `native` is the side's original-depth sidecar (null for 8-bit
    // sources); it is only consulted by the alpha filter, so a filter that is off costs
    // nothing and a filtered high-depth side never rounds its alpha to 8 bits first.
    [[nodiscard]] QImage
    channelView(const QImage& source, const QImage& native, int mode, bool primarySide) const;

    [[nodiscard]] ImagePairLoader::DecodePolicy currentDecodePolicy() const;
    void handleLoadFinished(ImagePairLoader::Result result);
    void handleDifferenceFinished(const ImagePairLoader::DifferenceResult& result,
                                  quint64 sourceGeneration,
                                  const QString& cacheKey);
    void
    commitLoadedPrimary(QImage image, QString label, QString identity, StillImageSourceInfo info);
    void
    commitLoadedSecondary(QImage image, QString label, QString identity, StillImageSourceInfo info);
    void commitLoadedPair(ImagePairLoader::Result result);
    // Single-side replace commits. Unlike commitLoadedPrimary these never clear the other
    // side or rewrite committedPairId.
    void
    commitReplacePrimary(QImage image, QString label, QString identity, StillImageSourceInfo info);
    void commitReplaceSecondary(QImage image,
                                QString label,
                                QString identity,
                                StillImageSourceInfo info);
    void refreshDifferenceAfterSideChange();
    // T6 observation-context retention for pair commits. hadPrimary/previousPrimarySize
    // describe the state before the commit; the helpers decide the mode and view the new
    // pair inherits so switching pairs keeps the user's observation position.
    [[nodiscard]] int retainedCompareModeFor(bool hadPrimary,
                                             const QSize& previousPrimarySize,
                                             const QSize& newPrimarySize,
                                             const QSize& newSecondarySize,
                                             QString* explanation) const;
    [[nodiscard]] bool retainsObservationPosition(bool hadPrimary) const noexcept;
    void cancelDifferenceRequest();
    void resetDifferenceState();
    void requestDifferenceForCurrentMode();
    void applyDifferenceResult(const ImagePairLoader::DifferenceResult& result);
    [[nodiscard]] QString differenceCacheKey() const;

    QImage primary_;
    QImage secondary_;
    // Original-depth RGBA64 sidecars (null for 8-bit sources). Owned with the same lifetime
    // as the display buffers so pixel sampling can always report honest code values.
    QImage primaryNative_;
    QImage secondaryNative_;
    QImage diff_;
    QString primaryPath_;
    QString secondaryPath_;
    QString errorText_;
    int compareMode_ = PrimaryOnly;
    int viewMode_ = RgbaView;
    qreal wipePosition_ = 0.5;
    int wipeOrientation_ = 0;
    qreal fadePosition_ = 0.5;
    int contentGeneration_ = 0;
    int maxAbsDifference_ = 0;
    double meanAbsDifference_ = 0.0;
    int peakAlphaDifference_ = 0;
    double meanAlphaDifference_ = 0.0;
    qint64 alphaChangedPixels_ = 0;
    // Source provenance for the honest "display-converted vs original code value" labeling.
    StillImageSourceInfo primaryInfo_;
    StillImageSourceInfo secondaryInfo_;
    // Derived channel views of primary_/secondary_ for the active viewMode_, invalidated by
    // generation bumps and view mode changes. Hover sampling derives only one pixel and never
    // enters this cache; image provider requests can still overlap one another.
    mutable std::mutex viewCacheMutex_;
    mutable QImage primaryViewCache_;
    mutable QImage secondaryViewCache_;
    mutable int viewCacheGeneration_ = -1;
    mutable int viewCacheMode_ = -1;
    // The alpha filter is part of the derived-view identity: changing the threshold has to
    // invalidate the cached views exactly like changing the observation mode does.
    mutable bool viewCacheFilterEnabled_ = false;
    mutable int viewCacheThreshold_ = -1;
    // COLOR4: colour conversion changes presented pixels without changing the decoded
    // generation, so it is part of the derived-view identity as well.
    mutable bool viewCacheColorEnabled_ = false;
    bool colorDisplayEnabled_ = true;
    // Row identity of the last atomically committed pair; -1 when no pair commit happened
    // (including after closeAll). Updated only by openPairAtomically/openPairImages and
    // closeAll so the canvas identity matches the folder list selection exactly.
    int committedPairId_ = -1;
    bool hasDiffResult_ = false;
    bool diffResampled_ = false;
    bool alphaDifferenceOnly_ = false;
    bool diffHasAlpha_ = false;
    bool resampleAllowed_ = false;
    int diffGain_ = kDefaultDifferenceGain;
    bool nativeStatsAvailable_ = false;
    int nativeMaxAbsDifference_ = 0;
    double nativeMeanAbsDifference_ = 0.0;
    int nativePeakAlphaDifference_ = 0;
    qint64 nativeChangedPixels_ = 0;
    qint64 nativeBeyondDisplayPixels_ = 0;
    qreal zoom_ = 1.0;
    qreal panX_ = 0.5;
    qreal panY_ = 0.5;
    QVariantMap cursorPixel_;
    // Numeric presentation and the alpha observation filter. Both are view state: they
    // change what is shown and what is copied, never a decoded value or a statistic.
    int numericFormat_ = IntegerCodeValues;
    bool alphaFilterEnabled_ = false;
    // kDefaultAlphaThreshold (0) is defined in the .cpp: the filter is off by default and
    // "off" and "on with T = 0" are different states (T = 0 still hides A = 0).
    int alphaThreshold_ = 0;
    QVariantMap frozenProbe_;
    std::unique_ptr<AsyncState> async_;
};

} // namespace dvs::ui
