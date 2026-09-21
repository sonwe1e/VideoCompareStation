#include "dvs/ui/DiagnosticsProbe.h"

#include "dvs/application/PlaybackTrace.h"

#include <cstdint>
#include <limits>

namespace dvs::ui {
namespace {

// TraceIdentity is coordinator-owned; a UI observation carries no session/playback identity, so it
// records the default (all-zero) scope and only a frame payload. Analyzers correlate it by
// timestamp against the pipeline events, exactly like the schema documents for UI-side events.
constexpr const char* const kGrabRequestedStage = "grab-requested";
constexpr const char* const kGrabCompletedStage = "grab-completed";

[[nodiscard]] std::uint64_t encodeValue(const double value) noexcept {
    constexpr auto kMaximum = (std::numeric_limits<std::uint64_t>::max)();
    if (!(value >= 0.0) || value > static_cast<double>(kMaximum)) {
        return kMaximum;
    }
    return static_cast<std::uint64_t>(value);
}

} // namespace

DiagnosticsProbe::DiagnosticsProbe(QObject* parent) : QObject(parent) {}

void DiagnosticsProbe::record(const QString& stage, const double value) {
    application::PlaybackTrace& trace = application::PlaybackTrace::instance();
    if (!trace.enabled()) {
        return;
    }
    application::TraceEventKind kind{};
    if (stage == QLatin1String{kGrabRequestedStage}) {
        kind = application::TraceEventKind::QmlGrabRequested;
    } else if (stage == QLatin1String{kGrabCompletedStage}) {
        kind = application::TraceEventKind::QmlGrabCompleted;
    } else {
        return;
    }
    trace.record(kind, application::TraceIdentity{}, encodeValue(value));
}

} // namespace dvs::ui
