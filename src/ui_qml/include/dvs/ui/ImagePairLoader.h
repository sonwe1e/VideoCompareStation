#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dvs::ui {

// Worker-off-thread still-image loader/difference service. Completion handlers always run on
// this object's owning thread, so callers can commit results in one step without touching
// QObject state from a decoder thread. A decoded QImage never changes after delivery.
class ImagePairLoader final : public QObject {
    Q_OBJECT

public:
    // Reads raw file bytes into an RGBA8 QImage. Injected by the app composition root so a
    // minimal deployment does not depend on Qt imageformat plugins.
    using ImageLoader = std::function<bool(const QByteArray&, QImage*, std::string*)>;
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
