#include "dvs/domain/ComparisonValidator.h"
#include "dvs/ui/ClipExportController.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dvs::ui {
namespace {

void ensureExportApplication() {
    if (QCoreApplication::instance()) {
        return;
    }
    static int argc = 1;
    static char name[] = "ClipExportControllerTests";
    static char* argv[] = {name, nullptr};
    static QCoreApplication application{argc, argv};
    static_cast<void>(application);
}

[[nodiscard]] bool waitForExport(ClipExportController& controller) {
    QElapsedTimer timer;
    timer.start();
    while (controller.busy() && timer.elapsed() < 1000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return !controller.busy();
}

[[nodiscard]] domain::CanonicalTimeline variableTimeline() {
    auto timeline = domain::FrameTimeline::create({domain::MediaTime{0},
                                                   domain::MediaTime{40'000},
                                                   domain::MediaTime{100'000},
                                                   domain::MediaTime{130'000}});
    if (!timeline) {
        throw std::runtime_error("Invalid export test timeline.");
    }
    return std::make_shared<const domain::FrameTimeline>(std::move(timeline.value()));
}

[[nodiscard]] std::shared_ptr<application::SessionSnapshot>
videoSnapshot(const bool variable = true, const char* const fileName = "canonical.mp4") {
    const auto rate = domain::RationalRate::create(25, 1).value();
    domain::MediaDescriptor descriptor{.duration = domain::MediaTime{0}};
    descriptor.normalizedPath = QDir::temp().filePath(QString::fromUtf8(fileName)).toStdWString();
    descriptor.extent = {16U, 16U};
    descriptor.frameRate = variable ? std::nullopt : std::optional{rate};
    descriptor.frameCount = {4, domain::FrameCountOrigin::kIndexed};
    descriptor.duration = domain::MediaTime{160'000};
    descriptor.codecId = "h264";
    descriptor.pixelFormatId = "yuv420p";
    descriptor.bitDepth = 8;
    descriptor.timingConfidence = variable ? domain::TimingConfidence::kVariableFrameRate
                                           : domain::TimingConfidence::kVerifiedCfr;
    auto reference = descriptor;
    reference.normalizedPath =
        QDir::temp().filePath(QStringLiteral("reference.mp4")).toStdWString();
    auto validated = domain::ComparisonValidator::validate(
        {{7U, domain::ComparisonRole::kPrediction, descriptor, "Timeline master"},
         {19U, domain::ComparisonRole::kReference, reference, "Reference"}});
    if (!validated) {
        throw std::runtime_error("Invalid export test comparison.");
    }
    auto snapshot = std::make_shared<application::SessionSnapshot>();
    snapshot->sessionId = domain::SessionId{23U};
    snapshot->sessionEpoch = domain::SessionEpoch{31U};
    snapshot->canonicalFrameCount = 4U;
    snapshot->canonicalTimeline = variable ? variableTimeline() : domain::CanonicalTimeline{rate};
    snapshot->validatedComparison =
        std::make_shared<const domain::ValidatedComparisonSet>(std::move(validated.value().set));
    snapshot->playbackRangeIn = domain::FrameId{1};
    snapshot->playbackRangeOut = domain::FrameId{2};
    return snapshot;
}

class RecordingExporter final : public application::IClipExporter {
public:
    std::vector<std::int64_t> keyframeTimes(const std::filesystem::path& path,
                                            const std::atomic_bool&) override {
        ++keyframeCalls;
        keyframeSource = path;
        return {0, 40'000};
    }

    application::ClipExportReport perform(const application::ClipExportJob& job,
                                          const std::atomic_bool&) override {
        ++performCalls;
        captured = job;
        captured->progress = {};
        return {.requestId = job.requestId, .outcome = outcome};
    }

    application::ClipExportOutcome outcome = application::ClipExportOutcome::kCompleted;
    std::atomic_int keyframeCalls{0};
    std::atomic_int performCalls{0};
    std::filesystem::path keyframeSource;
    std::optional<application::ClipExportJob> captured;
};

class ClipExportControllerTests : public testing::Test {
protected:
    void SetUp() override {
        ensureExportApplication();
        snapshot = videoSnapshot();
        exporter = std::make_shared<RecordingExporter>();
    }

    [[nodiscard]] ClipExportController::Dependencies dependencies() {
        return {.snapshot = [this] { return snapshot; }, .exporter = exporter};
    }

    void expectRejected() {
        ClipExportController controller{dependencies()};
        EXPECT_FALSE(controller.canExport());
        EXPECT_TRUE(controller.suggestedFileName().isEmpty());
        EXPECT_FALSE(controller.exportRange(target));
        EXPECT_FALSE(controller.busy());
        EXPECT_EQ(exporter->keyframeCalls.load(), 0);
        EXPECT_EQ(exporter->performCalls.load(), 0);
    }

    std::shared_ptr<application::SessionSnapshot> snapshot;
    std::shared_ptr<RecordingExporter> exporter;
    const QUrl target = QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("clip.mp4")));
};

struct ExportExample final {
    const char* name;
    bool variable;
    const char* sourceName;
    std::int64_t in;
    std::int64_t out;
    std::optional<std::int64_t> end;
    std::int64_t shift;
};

class ClipExportTimelineTests : public ClipExportControllerTests,
                                public testing::WithParamInterface<ExportExample> {};

TEST_P(ClipExportTimelineTests, CapturesAndExportsTheCanonicalTimeline) {
    const auto example = GetParam();
    snapshot = videoSnapshot(example.variable, example.sourceName);
    snapshot->playbackRangeIn = domain::FrameId{example.in};
    snapshot->playbackRangeOut = domain::FrameId{example.out};
    const auto original = snapshot;
    ClipExportController controller{dependencies()};
    int completions = 0;
    QObject::connect(&controller,
                     &ClipExportController::exportFinished,
                     &controller,
                     [&completions](const bool succeeded, const QString&) {
                         completions += succeeded ? 1 : -1;
                     });
    ASSERT_TRUE(controller.canExport());
    ASSERT_TRUE(controller.exportRange(target));
    // The worker must use the captured video, even if the current session changes immediately.
    snapshot = std::make_shared<application::SessionSnapshot>();
    ASSERT_TRUE(waitForExport(controller));
    ASSERT_EQ(completions, 1);
    ASSERT_TRUE(exporter->captured.has_value());
    EXPECT_EQ(exporter->keyframeCalls.load(), 1);
    EXPECT_EQ(exporter->performCalls.load(), 1);
    const auto& job = *exporter->captured;
    EXPECT_EQ(job.sessionId, original->sessionId);
    EXPECT_EQ(job.sessionEpoch, original->sessionEpoch);
    EXPECT_EQ(job.sourcePath, original->validatedComparison->canonicalDescriptor().normalizedPath);
    EXPECT_EQ(exporter->keyframeSource, job.sourcePath);
    EXPECT_EQ(job.outputPath, std::filesystem::path{target.toLocalFile().toStdWString()});
    const application::ClipExportPlan expected{
        .requestedRange = {domain::FrameId{example.in}, domain::FrameId{example.out}},
        .startMicroseconds = 40'000,
        .endMicroseconds = example.end,
        .startShiftMicroseconds = example.shift,
        .firstExportedFrame = domain::FrameId{1},
        .requestedFrameCount = example.out - example.in + 1,
    };
    EXPECT_EQ(job.plan, expected);
    EXPECT_EQ(controller.lastOutputPath(), QDir::toNativeSeparators(target.toLocalFile()));
}

INSTANTIATE_TEST_SUITE_P(
    Timelines,
    ClipExportTimelineTests,
    testing::Values(ExportExample{"VfrMiddle", true, "canonical.mp4", 1, 2, 130'000, 0},
                    ExportExample{"VfrTail", true, "canonical.mp4", 3, 3, std::nullopt, -90'000},
                    ExportExample{"CfrMiddle", false, "canonical.mp4", 1, 2, 120'000, 0},
                    ExportExample{"CfrTail", false, "canonical.mp4", 3, 3, 160'000, -80'000},
                    ExportExample{"NoExtension", true, "canonical", 1, 2, 130'000, 0}),
    [](const testing::TestParamInfo<ExportExample>& info) { return info.param.name; });

TEST_F(ClipExportControllerTests, SourcePathNamesCanonicalVideoRatherThanReference) {
    ClipExportController controller{dependencies()};
    EXPECT_EQ(controller.sourcePath(),
              QDir::toNativeSeparators(QDir::temp().filePath(QStringLiteral("canonical.mp4"))));
}

class ClipExportReadoutTests : public ClipExportControllerTests,
                               public testing::WithParamInterface<application::ClipExportOutcome> {
};

TEST_P(ClipExportReadoutTests, CapturesOneJobAndReturnsToCurrentProposal) {
    exporter->outcome = GetParam();
    ClipExportController controller{dependencies()};
    ASSERT_TRUE(controller.exportRange(target));
    snapshot = videoSnapshot(true, "next-folder/next-session.mp4");
    snapshot->playbackRangeIn = domain::FrameId{3};
    snapshot->playbackRangeOut = domain::FrameId{3};
    EXPECT_EQ(controller.sourcePath(),
              QDir::toNativeSeparators(QDir::temp().filePath(QStringLiteral("canonical.mp4"))));
    EXPECT_EQ(controller.rangeSummary(), QStringLiteral("入 2 · 出 3 · 2 帧"));
    EXPECT_EQ(controller.suggestedFileName(), QStringLiteral("canonical_clip_2-3.mp4"));
    EXPECT_EQ(controller.suggestedTarget(),
              QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("canonical_clip_2-3.mp4"))));
    ASSERT_TRUE(waitForExport(controller));
    EXPECT_EQ(controller.sourcePath(),
              QDir::toNativeSeparators(
                  QDir::temp().filePath(QStringLiteral("next-folder/next-session.mp4"))));
    EXPECT_EQ(controller.rangeSummary(), QStringLiteral("入 4 · 出 4 · 1 帧"));
    EXPECT_EQ(controller.suggestedFileName(), QStringLiteral("next-session_clip_4-4.mp4"));
    EXPECT_EQ(controller.suggestedTarget(),
              QUrl::fromLocalFile(
                  QDir::temp().filePath(QStringLiteral("next-folder/next-session_clip_4-4.mp4"))));
}

INSTANTIATE_TEST_SUITE_P(Outcomes,
                         ClipExportReadoutTests,
                         testing::Values(application::ClipExportOutcome::kCompleted,
                                         application::ClipExportOutcome::kCanceled,
                                         application::ClipExportOutcome::kFailed));

TEST_F(ClipExportControllerTests, SourcePathIsEmptyWithoutAnExportableRange) {
    ClipExportController controller{dependencies()};
    snapshot->playbackRangeOut = domain::FrameId{-1};
    EXPECT_TRUE(controller.sourcePath().isEmpty());
}

TEST_F(ClipExportControllerTests, ImageOnlyWorkspaceWithoutAVideoSnapshotIsRejected) {
    // Images are held by ImageReviewController, not the playback session's validated comparison.
    snapshot = std::make_shared<application::SessionSnapshot>();
    expectRejected();
}

TEST_F(ClipExportControllerTests, MissingSnapshotIsRejected) {
    snapshot.reset();
    expectRejected();
}

TEST_F(ClipExportControllerTests, MissingExporterIsRejected) {
    auto input = dependencies();
    input.exporter.reset();
    ClipExportController controller{std::move(input)};
    EXPECT_FALSE(controller.canExport());
    EXPECT_FALSE(controller.exportRange(target));
}

using InvalidSnapshot = void (*)(application::SessionSnapshot&);
struct InvalidExample final {
    const char* name;
    InvalidSnapshot invalidate;
};

class ClipExportInvalidStateTests : public ClipExportControllerTests,
                                    public testing::WithParamInterface<InvalidExample> {};

TEST_P(ClipExportInvalidStateTests, RejectsBeforeStartingTheWorker) {
    GetParam().invalidate(*snapshot);
    expectRejected();
}

INSTANTIATE_TEST_SUITE_P(
    InvalidStates,
    ClipExportInvalidStateTests,
    testing::Values(
        InvalidExample{"MissingComparison", [](auto& value) { value.validatedComparison.reset(); }},
        InvalidExample{"MissingTimeline", [](auto& value) { value.canonicalTimeline.reset(); }},
        InvalidExample{"ZeroFrames", [](auto& value) { value.canonicalFrameCount = 0U; }},
        InvalidExample{"UnrepresentableCount",
                       [](auto& value) {
                           value.canonicalFrameCount = std::numeric_limits<std::uint64_t>::max();
                       }},
        InvalidExample{"MissingIn", [](auto& value) { value.playbackRangeIn.reset(); }},
        InvalidExample{"MissingOut", [](auto& value) { value.playbackRangeOut.reset(); }},
        InvalidExample{"NegativeIn",
                       [](auto& value) { value.playbackRangeIn = domain::FrameId{-1}; }},
        InvalidExample{"ReversedRange",
                       [](auto& value) { value.playbackRangeIn = domain::FrameId{3}; }},
        InvalidExample{"OutOfRange",
                       [](auto& value) { value.playbackRangeOut = domain::FrameId{4}; }},
        InvalidExample{"NullVfr",
                       [](auto& value) {
                           value.canonicalTimeline = std::shared_ptr<const domain::FrameTimeline>{};
                       }},
        InvalidExample{"TruncatedVfrCount", [](auto& value) { value.canonicalFrameCount = 3U; }},
        InvalidExample{"ExcessVfrCount", [](auto& value) { value.canonicalFrameCount = 5U; }}),
    [](const testing::TestParamInfo<InvalidExample>& info) { return info.param.name; });

} // namespace
} // namespace dvs::ui
