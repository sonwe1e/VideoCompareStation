#pragma once

#include "dvs/ui/ImagePairLoader.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QUrl>
#include <QVariantMap>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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
    Q_PROPERTY(qreal wipePosition READ wipePosition WRITE setWipePosition NOTIFY viewChanged)
    Q_PROPERTY(qreal zoom READ zoom NOTIFY viewChanged)
    Q_PROPERTY(qreal panX READ panX NOTIFY viewChanged)
    Q_PROPERTY(qreal panY READ panY NOTIFY viewChanged)
    Q_PROPERTY(QVariantMap cursorPixel READ cursorPixel NOTIFY cursorPixelChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(int contentGeneration READ contentGeneration NOTIFY stateChanged)
    Q_PROPERTY(int maxAbsDifference READ maxAbsDifference NOTIFY stateChanged)
    Q_PROPERTY(double meanAbsDifference READ meanAbsDifference NOTIFY stateChanged)
    Q_PROPERTY(int committedPairId READ committedPairId NOTIFY stateChanged)
    Q_PROPERTY(bool hasDiffResult READ hasDiffResult NOTIFY stateChanged)
    Q_PROPERTY(bool diffResampled READ diffResampled NOTIFY stateChanged)
    Q_PROPERTY(bool alphaDifferenceOnly READ alphaDifferenceOnly NOTIFY stateChanged)
    Q_PROPERTY(
        bool resampleAllowed READ resampleAllowed WRITE setResampleAllowed NOTIFY stateChanged)
    Q_PROPERTY(QString diffScopeText READ diffScopeText NOTIFY stateChanged)
    Q_PROPERTY(bool openPending READ openPending NOTIFY stateChanged)
    Q_PROPERTY(bool diffPending READ diffPending NOTIFY stateChanged)

public:
    // Process-wide still-image decoder. Qt's imageformat plugins may omit PNG/JPEG on
    // minimal deploys; the app composition root injects an FFmpeg-backed loader that
    // consumes raw file bytes (Unicode-path safe).
    using StillImageLoader =
        std::function<bool(const QByteArray& fileBytes, QImage* image, std::string* error)>;
    static void setProcessStillImageLoader(StillImageLoader loader);
    // Optional header-only dimensions probe. Returning false means "unknown", not failure;
    // the loader still decodes and checks the decoded size afterwards.
    using StillImageProbe = std::function<bool(const QByteArray& fileBytes, QSize* size)>;
    static void setProcessStillImageProbe(StillImageProbe probe);

    enum CompareMode : int {
        PrimaryOnly = 0,
        SideBySide = 1,
        AbsDifference = 2,
        SignedDifference = 3,
        Highlight = 4,
        Wipe = 5,
    };
    Q_ENUM(CompareMode)

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
    [[nodiscard]] qreal wipePosition() const noexcept;
    void setWipePosition(qreal position);
    [[nodiscard]] qreal zoom() const noexcept;
    [[nodiscard]] qreal panX() const noexcept;
    [[nodiscard]] qreal panY() const noexcept;
    [[nodiscard]] QVariantMap cursorPixel() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] int contentGeneration() const noexcept;
    [[nodiscard]] int maxAbsDifference() const noexcept;
    [[nodiscard]] double meanAbsDifference() const noexcept;
    [[nodiscard]] int committedPairId() const noexcept;
    [[nodiscard]] bool hasDiffResult() const noexcept;
    [[nodiscard]] bool diffResampled() const noexcept;
    [[nodiscard]] bool alphaDifferenceOnly() const noexcept;
    [[nodiscard]] bool resampleAllowed() const noexcept;
    void setResampleAllowed(bool allowed);
    [[nodiscard]] QString diffScopeText() const;
    [[nodiscard]] bool openPending() const noexcept;
    [[nodiscard]] bool diffPending() const noexcept;

    Q_INVOKABLE bool openPrimary(const QUrl& url);
    Q_INVOKABLE bool openSecondary(const QUrl& url);
    // Opens both sides as one transaction: every validation runs before any state changes,
    // and on success the pair, paths and committedPairId switch in a single generation bump.
    // On failure the previous pair (or the explicit empty state) is fully retained and the
    // failure source is kept in errorText.
    Q_INVOKABLE bool
    openPairAtomically(const QUrl& primary, const QUrl& secondary, int pairId = -1);
    // Image-injection form of openPairAtomically (tests and non-file sources).
    bool openPairImages(QImage primary,
                        QString primaryLabel,
                        QImage secondary,
                        QString secondaryLabel,
                        int pairId = -1);
    Q_INVOKABLE void closeAll();
    Q_INVOKABLE void resetView();
    Q_INVOKABLE void zoomBy(qreal factor, qreal anchorNormalizedX, qreal anchorNormalizedY);
    Q_INVOKABLE void panBy(qreal deltaNormalizedX, qreal deltaNormalizedY);
    Q_INVOKABLE void setZoom(qreal value);
    Q_INVOKABLE QVariantMap samplePixel(int imageSlot, qreal imageX, qreal imageY) const;
    Q_INVOKABLE void updateCursorPixel(int imageSlot, qreal imageX, qreal imageY);
    Q_INVOKABLE void clearCursorPixel();
    Q_INVOKABLE QString imageUrl(int imageSlot) const;

    // T4 asynchronous entries. Each returns a positive request id when accepted; the final
    // commit or error is delivered through openFinished on the GUI thread. A newer request
    // invalidates older candidates, so a late N cannot replace N+2.
    Q_INVOKABLE int requestOpenPrimary(const QUrl& url);
    Q_INVOKABLE int requestOpenSecondary(const QUrl& url);
    Q_INVOKABLE int requestOpenPair(const QUrl& primary, const QUrl& secondary, int pairId = -1);
    Q_INVOKABLE void cancelPendingOpen();
    Q_INVOKABLE void cancelOpenRequest(int requestId);
    // One bounded neighbour read. It only warms the decoded-image cache; it never changes
    // the visible pair or committed identity.
    Q_INVOKABLE void prefetchPair(const QUrl& primary, const QUrl& secondary);
    Q_INVOKABLE QVariantMap asyncStats() const;
    Q_INVOKABLE void clearAsyncCaches();

    // Direct image injection (tests and non-file sources).
    bool openPrimaryImage(QImage image, QString pathLabel);
    bool openSecondaryImage(QImage image, QString pathLabel);

    // Used by ReviewImageProvider. Thread: GUI only (still images are CPU-resident).
    [[nodiscard]] QImage imageForSlot(int imageSlot) const;

signals:
    void stateChanged();
    void viewChanged();
    void cursorPixelChanged();
    // T4 completion contract: exactly one terminal per accepted asynchronous request.
    void openFinished(int requestId, int pairId, bool success, QString error);

private:
    struct AsyncState;

    void setError(QString text);
    void bumpGeneration();
    [[nodiscard]] static bool loadChecked(const QUrl& url, QImage* image, QString* error);
    [[nodiscard]] QImage displayImage(int slot) const;

    [[nodiscard]] ImagePairLoader::DecodePolicy currentDecodePolicy() const;
    void handleLoadFinished(ImagePairLoader::Result result);
    void handleDifferenceFinished(ImagePairLoader::DifferenceResult result);
    void commitLoadedPrimary(QImage image, QString label, QString identity);
    void commitLoadedSecondary(QImage image, QString label, QString identity);
    void commitLoadedPair(ImagePairLoader::Result result);
    void cancelDifferenceRequest();
    void resetDifferenceState();
    void requestDifferenceForCurrentMode();
    void applyDifferenceResult(const ImagePairLoader::DifferenceResult& result);
    [[nodiscard]] QString differenceCacheKey() const;

    QImage primary_;
    QImage secondary_;
    QImage diff_;
    QString primaryPath_;
    QString secondaryPath_;
    QString errorText_;
    int compareMode_ = PrimaryOnly;
    qreal wipePosition_ = 0.5;
    int contentGeneration_ = 0;
    int maxAbsDifference_ = 0;
    double meanAbsDifference_ = 0.0;
    // Row identity of the last atomically committed pair; -1 when no pair commit happened
    // (including after closeAll). Updated only by openPairAtomically/openPairImages and
    // closeAll so the canvas identity matches the folder list selection exactly.
    int committedPairId_ = -1;
    bool hasDiffResult_ = false;
    bool diffResampled_ = false;
    bool alphaDifferenceOnly_ = false;
    bool resampleAllowed_ = false;
    qreal zoom_ = 1.0;
    qreal panX_ = 0.5;
    qreal panY_ = 0.5;
    QVariantMap cursorPixel_;
    std::unique_ptr<AsyncState> async_;
};

} // namespace dvs::ui
