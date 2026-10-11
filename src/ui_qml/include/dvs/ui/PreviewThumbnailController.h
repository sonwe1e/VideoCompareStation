#pragma once

#include "dvs/application/PreviewThumbnail.h"
#include "dvs/application/SessionSnapshot.h"

#include <QObject>
#include <QSize>
#include <QUrl>
#include <QVariantList>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class QTimer;

namespace dvs::ui {

// GUI-thread cache of independently decoded timeline hover thumbnails. Requests are debounced
// and latest-wins so scrubbing the timeline never queues a decode storm. Images are served to
// QML through ReviewImageProvider-style URLs (`image://timeline-preview/<frame>?g=<generation>`)
// so the popup can show unplayed frames without grabToImage.
class PreviewThumbnailController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(int generation READ generation NOTIFY stateChanged)

public:
    struct RawThumbnail final {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;
    };
    struct Dependencies final {
        std::function<std::shared_ptr<const application::SessionSnapshot>()> snapshot;
        application::IPreviewThumbnailService* service = nullptr;
    };

    explicit PreviewThumbnailController(Dependencies dependencies, QObject* parent = nullptr);
    ~PreviewThumbnailController() override;

    PreviewThumbnailController(const PreviewThumbnailController&) = delete;
    PreviewThumbnailController& operator=(const PreviewThumbnailController&) = delete;
    PreviewThumbnailController(PreviewThumbnailController&&) = delete;
    PreviewThumbnailController& operator=(PreviewThumbnailController&&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] int generation() const noexcept;

    // Starts (or coalesces into) a decode for this canonical frame. Safe to call on every hover.
    Q_INVOKABLE void request(qint64 frame);
    // V04 nearby-frames card: replaces the low-priority prefetch batch with `frames` (already
    // cached frames are skipped). Batch work only drains while no interactive request is
    // pending or in flight, and an interactive request() preempts an in-flight batch decode
    // through the existing cancel(context) path - hover never waits behind a strip prefetch.
    Q_INVOKABLE void requestBatch(const QVariantList& frames);
    // Drops the queued batch and cancels the in-flight decode when it belongs to the batch
    // (a closing or re-targeted card must not keep decoding frames nobody will show).
    Q_INVOKABLE void cancelBatch();
    // Returns a cache URL when the frame is decoded; empty when still pending or unavailable.
    // GUI-thread reads first sync the cache to the active session identity, so thumbnails and
    // URLs from a previous session are never served after a session switch.
    Q_INVOKABLE QUrl urlForFrame(qint64 frame);
    Q_INVOKABLE bool hasThumbnail(qint64 frame);
    // Decoded pixel size of a cached frame (0x0 when unknown). The same-ROI strip needs the
    // source dimensions to place its crop rect without reading QML Image implicit sizes
    // (those depend on sourceClipRect itself, which would form a binding loop).
    Q_INVOKABLE QSize thumbnailSourceSize(qint64 frame);
    // Provider hook for `image://timeline-preview/<frame>` (RGBA8888 packed). Deliberately
    // session-agnostic: only reachable through URLs issued by urlForFrame for cached frames.
    [[nodiscard]] RawThumbnail rawForFrame(qint64 frame) const;

    void stop() noexcept;

Q_SIGNALS:
    void stateChanged();
    void thumbnailReady(qint64 frame);

private:
    class Sink;

    void drainSink();
    void onResult(application::PreviewThumbnailResult result);
    void scheduleRequest();
    // GUI-thread only. When the active session identity (sessionId + sessionEpoch) changes,
    // drops cached thumbnails, bumps the URL generation, and resets in-flight bookkeeping so
    // no frame from a previous session can be served for the new one (mirrors the QML grab
    // cache's generation reset in TimelineThumbnailCache).
    void syncSessionIdentity();

    Dependencies dependencies_;
    std::shared_ptr<Sink> sink_;
    QTimer* debounce_ = nullptr;
    mutable std::mutex cacheMutex_;
    std::map<qint64, RawThumbnail> cache_;
    qint64 pendingFrame_ = -1;
    qint64 inflightFrame_ = -1;
    std::optional<application::RequestContext> inflightContext_;
    // V04 low-priority batch state: frames still to prefetch (FIFO) and whether the current
    // in-flight decode belongs to the batch (it is then preemptible by interactive requests
    // and cancellable by cancelBatch()).
    std::vector<qint64> batchQueue_;
    bool inflightIsBatch_ = false;
    quint64 nextRequestId_ = 1U;
    int generation_ = 1;
    domain::SessionId cachedSessionId_{0};
    domain::SessionEpoch cachedSessionEpoch_{0};
    bool sessionIdentityKnown_ = false;
    bool stopped_ = false;
};

} // namespace dvs::ui
