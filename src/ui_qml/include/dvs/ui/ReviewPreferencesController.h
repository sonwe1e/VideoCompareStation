#pragma once

#include "dvs/application/Ports.h"
#include "dvs/presentation/ComparisonContract.h"

#include <QObject>

#include <memory>

namespace dvs::ui {

class ReviewPreferencesController final : public QObject {
    Q_OBJECT

    Q_PROPERTY(
        int shortcutPreset READ shortcutPreset WRITE setShortcutPreset NOTIFY preferencesChanged)
    Q_PROPERTY(bool dropFrameTimecode READ dropFrameTimecode WRITE setDropFrameTimecode NOTIFY
                   preferencesChanged)
    Q_PROPERTY(int viewMode READ viewModeCode WRITE setViewModeCode NOTIFY preferencesChanged)
    Q_PROPERTY(int differenceMetric READ differenceMetricCode WRITE setDifferenceMetricCode NOTIFY
                   preferencesChanged)
    Q_PROPERTY(int differenceGain READ differenceGainCode WRITE setDifferenceGainCode NOTIFY
                   preferencesChanged)
    Q_PROPERTY(int differenceEdge READ differenceEdgeCode WRITE setDifferenceEdgeCode NOTIFY
                   preferencesChanged)
    Q_PROPERTY(int differenceFilter READ differenceFilterCode WRITE setDifferenceFilterCode NOTIFY
                   preferencesChanged)
    Q_PROPERTY(int oscMode READ oscMode WRITE setOscMode NOTIFY preferencesChanged)
    Q_PROPERTY(int playbackContinuityPolicy READ playbackContinuityPolicy WRITE
                   setPlaybackContinuityPolicy NOTIFY preferencesChanged)
    Q_PROPERTY(int defaultPairPolicy READ defaultPairPolicy WRITE setDefaultPairPolicy NOTIFY
                   preferencesChanged)

public:
    using ViewMode = presentation::ViewMode;
    using DifferenceMetric = presentation::DifferenceMetric;
    using DifferenceGain = presentation::DifferenceGain;
    using DifferenceEdge = presentation::DifferenceEdge;
    using DifferenceFilter = presentation::DifferenceFilter;

    explicit ReviewPreferencesController(
        std::shared_ptr<application::ISettingsRepository> repository, QObject* parent = nullptr);
    ~ReviewPreferencesController() override;

    ReviewPreferencesController(const ReviewPreferencesController&) = delete;
    ReviewPreferencesController& operator=(const ReviewPreferencesController&) = delete;
    ReviewPreferencesController(ReviewPreferencesController&&) = delete;
    ReviewPreferencesController& operator=(ReviewPreferencesController&&) = delete;

    [[nodiscard]] int shortcutPreset() const noexcept;
    [[nodiscard]] bool dropFrameTimecode() const noexcept;
    [[nodiscard]] ViewMode viewMode() const noexcept;
    [[nodiscard]] DifferenceMetric differenceMetric() const noexcept;
    [[nodiscard]] DifferenceGain differenceGain() const noexcept;
    [[nodiscard]] DifferenceEdge differenceEdge() const noexcept;
    [[nodiscard]] DifferenceFilter differenceFilter() const noexcept;
    [[nodiscard]] int viewModeCode() const noexcept;
    [[nodiscard]] int differenceMetricCode() const noexcept;
    [[nodiscard]] int differenceGainCode() const noexcept;
    [[nodiscard]] int differenceEdgeCode() const noexcept;
    [[nodiscard]] int differenceFilterCode() const noexcept;
    [[nodiscard]] int oscMode() const noexcept;
    // C-07: domain::PlaybackContinuityPolicy code (0 ReviewEveryFrame, 1 RealTime, 2 Contextual).
    [[nodiscard]] int playbackContinuityPolicy() const noexcept;
    // C-02: domain::DefaultPairPolicy code (0 Ref+First, 1 LastTwo, 2 PreserveIfAvailable).
    [[nodiscard]] int defaultPairPolicy() const noexcept;

    void setShortcutPreset(int value);
    void setDropFrameTimecode(bool value);
    void setViewMode(ViewMode value);
    void setDifferenceMetric(DifferenceMetric value);
    void setDifferenceGain(DifferenceGain value);
    void setDifferenceEdge(DifferenceEdge value);
    void setDifferenceFilter(DifferenceFilter value);
    void setViewModeCode(int value);
    void setDifferenceMetricCode(int value);
    void setDifferenceGainCode(int value);
    void setDifferenceEdgeCode(int value);
    void setDifferenceFilterCode(int value);
    void setOscMode(int value);
    void setPlaybackContinuityPolicy(int value);
    void setDefaultPairPolicy(int value);

    Q_INVOKABLE void stop() noexcept;
    void processRepositoryEvents() noexcept;

Q_SIGNALS:
    void preferencesChanged();

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::ui
