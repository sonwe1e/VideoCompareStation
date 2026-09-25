#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dvs::ui {

// Display-only amplification bounds for the rendered difference image. Statistics are always
// accumulated from the raw 8-bit code deltas while the pair analysis runs, so changing the gain
// changes the picture and never a number. Signed difference, highlight and alpha difference all
// share the same gain so different candidates stay visually comparable.
inline constexpr int kMinimumDifferenceGain = 1;
inline constexpr int kMaximumDifferenceGain = 16;
inline constexpr int kDefaultDifferenceGain = 4;

// Working-set accounting for still-image comparison. The previous check counted one
// width*height*4 display buffer per side, so the converted RGBA64 sidecars, the shared analysis
// field and the derived difference image never entered the budget.
struct ImageWorkingSetEstimate final {
    qint64 displayBytes = 0;  // decoded RGBA8 display buffers
    qint64 nativeBytes = 0;   // converted RGBA64 sidecars of high-bit-depth sources
    qint64 analysisBytes = 0; // per-pair delta field + sign map (pair estimates only)
    qint64 derivedBytes = 0;  // rendered difference image (pair estimates only)
    qint64 resampleBytes = 0; // extra scaled copy when a mismatched pair is resampled
    qint64 totalBytes = 0;
};

[[nodiscard]] ImageWorkingSetEstimate
estimateSingleImageWorkingSet(int width, int height, bool nativeSidecar) noexcept;
[[nodiscard]] ImageWorkingSetEstimate
estimatePairWorkingSet(QSize primary, QSize secondary, bool nativeSidecars, bool resample) noexcept;

// Hard ceiling for one compared pair's whole working set. The largest pair the per-image check
// accepts (7680x4320 with both RGBA64 sidecars, analysis field and derived image) needs about
// 1.1 GiB, so this ceiling does not reject anything the older per-image check accepted.
inline constexpr qint64 kImagePairWorkingSetBudgetBytes = 1536LL * 1024LL * 1024LL;

// Provenance of a decoded still image. The display buffer is always RGBA8 with straight
// (unassociated) alpha; these fields describe the *source* the buffer was converted from, so
// the UI never implies that a sampled pixel value is the file's original code value.
struct StillImageSourceInfo final {
    // Source bits per component before the display conversion (8/10/12/16…).
    int bitDepth = 8;
    // Decoded source channel count (1 = gray, 3 = RGB, 4 = RGBA).
    int channels = 4;
    // The decoded source frame carried an alpha channel.
    bool hasAlpha = false;
    // The decoded buffer's alpha is straight (unassociated). Premultiplied sources are
    // interpreted as straight on decode; the two representations must never be subtracted
    // against each other.
    bool straightAlpha = true;
    // FFmpeg pixel format name of the decoded source frame, e.g. "rgba", "gray16be",
    // "rgb48be", "yuvj420p"; empty when unknown.
    QString sourceFormat;
    // FFmpeg AVCOL_RANGE of the source frame, or -1 when unknown.
    int colorRange = -1;
    // True when the source differs from the display buffer (bit depth, channels or color
    // space), i.e. sampled pixels are display-converted values, not original code values.
    bool displayConverted = false;
};

// Worker-off-thread still-image loader/difference service. Completion handlers always run on
// this object's owning thread, so callers can commit results in one step without touching
// QObject state from a decoder thread. A decoded QImage never changes after delivery.
class ImagePairLoader final : public QObject {
    Q_OBJECT

public:
    // Reads raw file bytes into an RGBA8 QImage plus its source provenance. Injected by the
    // app composition root so a minimal deployment does not depend on Qt imageformat plugins.
    // nativeImage, when non-null on return, carries the original-depth RGBA64 samples (empty
    // for 8-bit sources whose display buffer already holds the original code values).
    using ImageLoader = std::function<bool(const QByteArray&,
                                           QImage* image,
                                           QImage* nativeImage,
                                           StillImageSourceInfo* info,
                                           std::string* error)>;
    // Returns true when the header dimensions could be read without decoding. False means
    // the header is unknown; the loader then falls back to the decoded image's dimensions.
    using ImageProbe = std::function<bool(const QByteArray&, QSize*)>;

    struct DecodePolicy final {
        std::uint64_t revision = 0;
        ImageLoader loader;
        ImageProbe probe;
    };

    struct Result final {
        quint64 requestId = 0;
        int pairId = -1;
        bool pair = false;
        bool secondaryOnly = false;
        QImage primary;
        QImage secondary;
        // Original-depth RGBA64 sidecars; null QImages when the source is 8-bit.
        QImage primaryNative;
        QImage secondaryNative;
        StillImageSourceInfo primaryInfo;
        StillImageSourceInfo secondaryInfo;
        QString primaryLabel;
        QString secondaryLabel;
        QString primaryIdentity;
        QString secondaryIdentity;
        // 0 = A/primary, 1 = B/secondary, -1 = no side-specific failure.
        int failedSide = -1;
        QString error;

        [[nodiscard]] bool succeeded() const noexcept {
            return error.isEmpty();
        }
    };
    using ResultHandler = std::function<void(Result)>;

    struct DifferenceResult final {
        quint64 requestId = 0;
        QImage image;
        // Display-only amplification used for image. Every statistic below is a raw 8-bit code
        // delta and is independent of the gain.
        int gain = kDefaultDifferenceGain;
        int maxAbsDifference = 0;
        double meanAbsDifference = 0.0;
        // Alpha statistics cover the straight (unassociated) alpha channel of the decoded
        // RGBA8 buffers. They are always accumulated, so an alpha-only regression is visible
        // even when the RGB channels match.
        int peakAlphaDifference = 0;
        double meanAlphaDifference = 0.0;
        qint64 alphaChangedPixels = 0;
        // True when either side has at least one non-opaque pixel.
        bool hasAlpha = false;
        bool resampled = false;
        bool alphaDifferenceOnly = false;
        // The pair analysis (delta field plus all statistics above) was reused from the cache
        // instead of being recomputed from both sources; only the requested display variant was
        // re-rendered.
        bool analysisReused = false;
        // High-depth statistics over the converted RGBA64 sidecars. They are available only when
        // both sides carry a same-size sidecar and no resampling took place. The values are
        // decoder-converted 16-bit code values, never raw file planes.
        bool nativeAvailable = false;
        int nativeMaxAbsDifference = 0;
        double nativeMeanAbsDifference = 0.0;
        int nativePeakAlphaDifference = 0;
        qint64 nativeChangedPixels = 0;
        // Pixels whose RGBA8 display samples are equal while the 16-bit sidecars differ. This is
        // the case the display buffer cannot show and the numbers must still report.
        qint64 nativeBeyondDisplayPixels = 0;
        bool displayEqualButNativeDifferent = false;
        QString error;

        [[nodiscard]] bool succeeded() const noexcept {
            return error.isEmpty() && !image.isNull();
        }
    };
    using DifferenceHandler = std::function<void(DifferenceResult)>;

    struct Stats final {
        qint64 cacheBytes = 0;
        qint64 cacheBudgetBytes = 0;
        int cacheEntries = 0;
        quint64 cacheHits = 0;
        quint64 cacheMisses = 0;
        int activeRequests = 0;
        int maxThreadCount = 0;
        int pendingRequests = 0;
        // Per-pair analysis reuse: how often the delta field and statistics were built from the
        // sources versus reused for a different mode or gain.
        quint64 analysisRuns = 0;
        quint64 analysisReuses = 0;
        quint64 renderRuns = 0;
        // Whole-working-set accounting for the pair currently held by the analysis cache.
        ImageWorkingSetEstimate workingSet;
        qint64 workingSetBudgetBytes = kImagePairWorkingSetBudgetBytes;
    };

    // Display options for one difference render. The analysis behind it (delta field and every
    // statistic) depends only on the two images and the resampling decision, so switching mode
    // or gain reuses it.
    struct DifferenceOptions final {
        int compareMode = 0;
        bool resample = false;
        int gain = kDefaultDifferenceGain;
    };

    explicit ImagePairLoader(QObject* parent = nullptr);
    ~ImagePairLoader() override;

    // All request methods return immediately with a monotonically increasing request id.
    // The completion handler is invoked later on this object's thread.
    quint64 requestPrimary(const QUrl& primary,
                           int pairId,
                           const DecodePolicy& policy,
                           ResultHandler handler);
    quint64 requestSecondary(const QUrl& secondary,
                             int pairId,
                             const DecodePolicy& policy,
                             ResultHandler handler);
    quint64 requestPair(const QUrl& primary,
                        const QUrl& secondary,
                        int pairId,
                        const DecodePolicy& policy,
                        ResultHandler handler);
    // Low-priority neighbour read: decodes and caches, never changes caller-visible state.
    // A user request cancels outstanding prefetches.
    quint64 prefetchPair(const QUrl& primary, const QUrl& secondary, const DecodePolicy& policy);
    // Renders one difference display variant. The two source buffers never change after
    // delivery, so the analysis behind the image (per-pixel delta field plus every statistic) is
    // computed once per pair and reused across mode and gain changes.
    quint64 requestDifference(QImage primary,
                              QImage secondary,
                              QImage primaryNative,
                              QImage secondaryNative,
                              const DifferenceOptions& options,
                              DifferenceHandler handler);
    // Historical signature: no high-depth sidecars, default gain.
    quint64 requestDifference(QImage primary,
                              QImage secondary,
                              int compareMode,
                              bool resample,
                              DifferenceHandler handler);
    // Drops the cached per-pair analysis. Called when the committed pair changes so the field
    // and its source buffers are not retained past their use.
    void clearAnalysisCache();

    // Test seam: copied into each request, invoked on its worker before each row of the pair
    // analysis (the pass that reads both sources). A render that reuses a cached analysis does
    // not invoke it. Production leaves this empty. The observer must not touch GUI state.
    void setDifferenceRowObserverForTesting(std::function<void(int)> observer);

    void cancel(quint64 requestId);
    void cancelAll();
    void cancelPrefetches();

    void setCacheBudgetBytes(qint64 bytes);
    [[nodiscard]] qint64 cacheBudgetBytes() const noexcept;
    // Whole-working-set ceiling for one compared pair, including sidecars, analysis field and
    // derived image. Configurable so tests can prove a pair is refused before it allocates.
    void setWorkingSetBudgetBytes(qint64 bytes);
    [[nodiscard]] qint64 workingSetBudgetBytes() const noexcept;
    void clearCache();
    [[nodiscard]] Stats stats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::ui
