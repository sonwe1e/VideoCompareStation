#pragma once

#include "dvs/application/PairMetrics.h"
#include "dvs/application/SessionSnapshot.h"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class QTimer;

namespace dvs::ui {

class ReviewController;

// GUI-thread projection of pair-metrics samples for the active comparison pair. The controller
// owns the worker-to-GUI bridge: batches are queued by the sink on the metrics worker and drained
// on the GUI thread, and every batch is validated against the current session scope before it
// enters the cache, so stale results are always dropped.
class PairMetricsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool laneEnabled READ laneEnabled WRITE setLaneEnabled NOTIFY stateChanged)
    Q_PROPERTY(bool sampling READ sampling NOTIFY stateChanged)
    Q_PROPERTY(bool hasCurrentSample READ hasCurrentSample NOTIFY stateChanged)
    Q_PROPERTY(bool currentComparable READ currentComparable NOTIFY stateChanged)
    Q_PROPERTY(qreal currentMae READ currentMae NOTIFY stateChanged)
    Q_PROPERTY(qreal currentMse READ currentMse NOTIFY stateChanged)
    Q_PROPERTY(qreal currentPsnrDb READ currentPsnrDb NOTIFY stateChanged)
    Q_PROPERTY(qreal currentMaxAbsError READ currentMaxAbsError NOTIFY stateChanged)
    Q_PROPERTY(qreal currentMismatchRatio READ currentMismatchRatio NOTIFY stateChanged)
    Q_PROPERTY(qulonglong currentMismatchPixels READ currentMismatchPixels NOTIFY stateChanged)
    Q_PROPERTY(qulonglong currentPixelCount READ currentPixelCount NOTIFY stateChanged)
    Q_PROPERTY(QString metricId READ metricId NOTIFY stateChanged)
    Q_PROPERTY(QString errorKey READ errorKey NOTIFY stateChanged)
    Q_PROPERTY(int threshold READ threshold WRITE setThreshold NOTIFY thresholdChanged)
    Q_PROPERTY(qint64 sampleCount READ sampleCount NOTIFY samplesChanged)
    Q_PROPERTY(qint64 sampleFirstFrame READ sampleFirstFrame NOTIFY samplesChanged)
    Q_PROPERTY(qint64 sampleLastFrame READ sampleLastFrame NOTIFY samplesChanged)
    Q_PROPERTY(qreal sampleMaxMae READ sampleMaxMae NOTIFY samplesChanged)

public:
    struct Dependencies final {
        std::function<std::shared_ptr<const application::SessionSnapshot>()> snapshot;
        application::IPairMetricsService* service = nullptr;
    };

    explicit PairMetricsController(Dependencies dependencies, QObject* parent = nullptr);
    ~PairMetricsController() override;

    PairMetricsController(const PairMetricsController&) = delete;
    PairMetricsController& operator=(const PairMetricsController&) = delete;
    PairMetricsController(PairMetricsController&&) = delete;
    PairMetricsController& operator=(PairMetricsController&&) = delete;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool laneEnabled() const noexcept;
    void setLaneEnabled(bool value);
    [[nodiscard]] bool sampling() const noexcept;
    [[nodiscard]] bool hasCurrentSample() const noexcept;
    [[nodiscard]] bool currentComparable() const noexcept;
    [[nodiscard]] qreal currentMae() const noexcept;
    [[nodiscard]] qreal currentMse() const noexcept;
    [[nodiscard]] qreal currentPsnrDb() const noexcept;
    [[nodiscard]] qreal currentMaxAbsError() const noexcept;
    [[nodiscard]] qreal currentMismatchRatio() const noexcept;
    [[nodiscard]] qulonglong currentMismatchPixels() const noexcept;
    [[nodiscard]] qulonglong currentPixelCount() const noexcept;
    [[nodiscard]] QString metricId() const;
    [[nodiscard]] QString errorKey() const;
    [[nodiscard]] int threshold() const noexcept;
    void setThreshold(int value);
    [[nodiscard]] qint64 sampleCount() const noexcept;
    [[nodiscard]] qint64 sampleFirstFrame() const noexcept;
    [[nodiscard]] qint64 sampleLastFrame() const noexcept;
    [[nodiscard]] qreal sampleMaxMae() const noexcept;

    // Connects to the review controller's state/frame notifications. Safe to call once from the
    // composition root; never stores the pointer beyond the connection scope.
    void attachReviewController(ReviewController& controller);

    // Manual refresh for tests and adapters without a review controller.
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void clear();
    Q_INVOKABLE qreal maeAt(qint64 frame) const;
    Q_INVOKABLE bool comparableAt(qint64 frame) const;
    // Local maxima of comparable MAE samples, strongest first, at most maximumCount entries.
    Q_INVOKABLE QVariantList peakFrames(int maximumCount, qreal neighborhoodFrames) const;
    // Bucket-reduced sample series for drawing: at most maximumPoints entries of
    // {frame, mae, comparable}. Each bucket reports the maximum comparable MAE inside it so a
    // narrow spike never disappears at low zoom.
    Q_INVOKABLE QVariantList samplePoints(int maximumPoints) const;

    // Shutdown-only: cancels the in-flight request and closes the sink. No further batches are
    // accepted afterwards.
    void stop() noexcept;

Q_SIGNALS:
    void stateChanged();
    void samplesChanged();
    void thresholdChanged();

private:
    class Sink final : public application::IPairMetricsSink {
    public:
        explicit Sink(PairMetricsController& owner);
        ~Sink() override;

        void onPairMetricsBatch(application::PairMetricsBatch batch) override;
        void onPairMetricsFailure(application::PairMetricsFailure failure) override;
        void close() noexcept;
        [[nodiscard]] std::optional<application::PairMetricsFailure> takeFailure();
        [[nodiscard]] std::vector<application::PairMetricsBatch> takeBatches();

    private:
        PairMetricsController* owner_;
        std::mutex mutex_;
        bool closed_ = false;
        std::deque<application::PairMetricsBatch> batches_;
        std::optional<application::PairMetricsFailure> failure_;
    };

    struct Sample final {
        bool comparable = false;
        domain::PixelDifferenceMetrics metrics{};
    };

    struct Scope final {
        domain::SessionId sessionId{0};
        domain::SessionEpoch sessionEpoch{0};
        domain::SourceId firstSource = 0;
        domain::SourceId secondSource = 0;
        std::uint64_t alignmentRevision = 0U;
        int threshold = 0;

        [[nodiscard]] bool operator==(const Scope&) const noexcept = default;
    };

    void onReviewStateChanged();
    void onReviewFrameStateChanged();
    void scheduleRequest();
    void submitRequest();
    void drainSink();
    void applyScopeChange(const std::optional<Scope>& next);
    void resetInflight() noexcept;
    [[nodiscard]] const Sample* currentSample() const noexcept;

    Dependencies dependencies_;
    std::shared_ptr<Sink> sink_;
    std::map<std::int64_t, Sample> samples_;
    std::optional<Scope> scope_;
    // In-flight request range for deduplication while a job is decoding.
    std::int64_t inflightFirst_ = -1;
    std::int64_t inflightLast_ = -1;
    application::PlaybackRequestContext lastRequestContext_{
        application::RequestContext{
            domain::SessionId{0}, domain::SessionEpoch{0}, domain::RequestId{0}},
        domain::PlaybackGeneration{0}};
    bool hasLastRequest_ = false;
    std::uint64_t nextRequestId_ = 1U;
    bool laneEnabled_ = false;
    bool sampling_ = false;
    bool available_ = false;
    QString errorKey_;
    int threshold_ = 0;
    qint64 sampleFirstFrame_ = -1;
    qint64 sampleLastFrame_ = -1;
    qreal sampleMaxMae_ = 0.0;
    QString metricId_;
    QTimer* requestTimer_ = nullptr;
};

} // namespace dvs::ui
