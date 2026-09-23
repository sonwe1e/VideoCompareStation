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
    quint64 requestDifference(QImage primary,
                              QImage secondary,
                              int compareMode,
                              bool resample,
                              DifferenceHandler handler);

    // Test seam: copied into each request, invoked on its worker before each diff row.
    // Production leaves this empty. The observer must not touch GUI state.
    void setDifferenceRowObserverForTesting(std::function<void(int)> observer);

    void cancel(quint64 requestId);
    void cancelAll();
    void cancelPrefetches();

    void setCacheBudgetBytes(qint64 bytes);
    [[nodiscard]] qint64 cacheBudgetBytes() const noexcept;
    void clearCache();
    [[nodiscard]] Stats stats() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::ui
