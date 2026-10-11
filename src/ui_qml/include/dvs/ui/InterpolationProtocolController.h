#pragma once

#include "dvs/application/InterpolationProtocolImport.h"
#include "dvs/domain/InterpolationProtocol.h"

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <atomic>
#include <memory>
#include <optional>

namespace dvs::ui {

class ReviewController;

// P03 protocol import and generated-frame navigation. The controller holds at most one loaded
// domain::InterpolationProtocol and projects the current frame's provenance for the transport
// cluster. Navigation is an explicit user action expressed through ReviewController::seekFrame,
// i.e. the existing SeekFrameCommand chain via PlaybackCoordinator; normal playback speed and
// cadence are never touched. With no protocol loaded every navigation entry point stays
// disabled and the controller changes nothing about the review session.
//
// loadProtocol() reads and decodes the document on a QThreadPool worker so a multi-megabyte
// explicit manifest cannot stall the GUI thread; results carry a monotonically increasing
// request id, and a result whose id is no longer current (superseded load, clearProtocol, or
// shutdown) is dropped instead of applied. A failed load never replaces the protocol already
// in place.
class InterpolationProtocolController final : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool protocolLoaded READ protocolLoaded NOTIFY protocolChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY protocolChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY protocolChanged)
    Q_PROPERTY(QString currentOriginKind READ currentOriginKind NOTIFY currentOriginChanged)
    Q_PROPERTY(QString currentOriginBadge READ currentOriginBadge NOTIFY currentOriginChanged)
    Q_PROPERTY(bool canGoNextGenerated READ canGoNextGenerated NOTIFY currentOriginChanged)
    Q_PROPERTY(bool canGoPreviousGenerated READ canGoPreviousGenerated NOTIFY currentOriginChanged)

public:
    explicit InterpolationProtocolController(
        application::IInterpolationProtocolRepository* repository, QObject* parent = nullptr);
    ~InterpolationProtocolController() override;

    InterpolationProtocolController(const InterpolationProtocolController&) = delete;
    InterpolationProtocolController& operator=(const InterpolationProtocolController&) = delete;
    InterpolationProtocolController(InterpolationProtocolController&&) = delete;
    InterpolationProtocolController& operator=(InterpolationProtocolController&&) = delete;

    // The seek target projection reads the current frame from this controller and jumps via its
    // seekFrame(); required before navigation can do anything.
    void setReviewController(ReviewController* controller) noexcept;

    [[nodiscard]] bool protocolLoaded() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString lastError() const;
    // "retained" | "generated" | "unknown" | "missing", or "" when no protocol is loaded or no
    // frame has been presented yet.
    [[nodiscard]] QString currentOriginKind() const;
    // Human-readable provenance badge; text alone carries the meaning (never colour only).
    [[nodiscard]] QString currentOriginBadge() const;
    [[nodiscard]] bool canGoNextGenerated() const;
    [[nodiscard]] bool canGoPreviousGenerated() const;

    // Starts the asynchronous import. Returns false only when the request could not be started
    // (no repository, non-local url); decode failures arrive asynchronously via lastError.
    Q_INVOKABLE bool loadProtocol(const QUrl& url);
    // Rollback path: closes protocol navigation and returns to protocol-free review.
    Q_INVOKABLE void clearProtocol();
    // Seeks to the next/previous generated output position from the current frame. Returns
    // false when there is no protocol, no target in that direction, or the coordinator-side
    // seek was rejected.
    Q_INVOKABLE bool nextGeneratedFrame();
    Q_INVOKABLE bool previousGeneratedFrame();
    Q_INVOKABLE QString originKindAtCurrentFrame() const;
    // P06 (D02): per-frame provenance for the diagnostic card's film strip across
    // [fromFrame, toFrame] on the canonical timeline, in order. Frames beyond the loaded
    // protocol report kind "outside" so the strip can still align its cells with per-side
    // presentation facts. The "frame" field is the zero-based internal id (the value
    // seekFrame accepts); every display*-prefixed field follows the one-based UI text
    // convention, and n/a numerics are -1 so QML never branches on key presence. Returns an
    // empty list when no protocol is loaded - a missing protocol only drops the badges, it
    // never blocks the card.
    Q_INVOKABLE QVariantList originSummariesInRange(qint64 fromFrame, qint64 toFrame) const;

Q_SIGNALS:
    void protocolChanged();
    void currentOriginChanged();

private:
    // Shared with the worker so cancellation survives without touching this from its thread.
    struct LoadRequest final {
        std::atomic<bool> cancelled{false};
    };

    void deliverLoadResult(quint64 requestId, application::InterpolationProtocolIoResult result);
    void refreshOriginProjection();
    [[nodiscard]] QString buildStatusText() const;

    application::IInterpolationProtocolRepository* repository_ = nullptr;
    ReviewController* review_ = nullptr;
    std::optional<domain::InterpolationProtocol> protocol_;
    // Identity of the most recent load request; increments on every loadProtocol() and on
    // clearProtocol() so stale worker results can be recognized and dropped.
    quint64 loadRequestId_ = 0U;
    std::shared_ptr<LoadRequest> inFlightLoad_;
    QString lastError_;
    QString originKind_;
    QString originBadge_;
};

} // namespace dvs::ui
