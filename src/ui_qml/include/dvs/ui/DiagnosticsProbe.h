#pragma once

#include <QObject>
#include <QString>

namespace dvs::ui {

// QML-facing diagnostic bridge. It forwards UI-originated observation events into the bounded
// application PlaybackTrace buffer so an evidence gate can correlate a QML scene-graph operation
// with the playback pipeline timing around it. The bridge holds no playback state and changes no
// behavior: with tracing disabled every call is one atomic load and a branch.
//
// Stages are fixed names rather than an open string space so the trace vocabulary stays explicit
// (see docs/engineering/trace-schema.md).
class DiagnosticsProbe final : public QObject {
    Q_OBJECT

public:
    explicit DiagnosticsProbe(QObject* parent = nullptr);

    // Records one UI observation. `stage` is a stable lowercase identifier; `value` carries the
    // stage-specific payload (for example the canonical frame number the UI was showing).
    Q_INVOKABLE void record(const QString& stage, double value = -1.0);

private:
    Q_DISABLE_COPY_MOVE(DiagnosticsProbe)
};

} // namespace dvs::ui
