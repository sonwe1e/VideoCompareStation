#pragma once

#include "dvs/application/PreviewThumbnail.h"
#include "dvs/application/SessionSnapshot.h"

#include <QObject>
#include <QUrl>

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
    // Returns a cache URL when the frame is decoded; empty when still pending or unavailable.
    // GUI-thread reads first sync the cache to the active session identity, so thumbnails and
    // URLs from a previous session are never served after a session switch.
    Q_INVOKABLE QUrl urlForFrame(qint64 frame);
    Q_INVOKABLE bool hasThumbnail(qint64 frame);
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
    quint64 nextRequestId_ = 1U;
    int generation_ = 1;
    domain::SessionId cachedSessionId_{0};
    domain::SessionEpoch cachedSessionEpoch_{0};
    bool sessionIdentityKnown_ = false;
    bool stopped_ = false;
};

} // namespace dvs::ui
