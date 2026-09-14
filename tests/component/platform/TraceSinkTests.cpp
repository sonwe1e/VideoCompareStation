#include "dvs/platform/TraceSink.h"
#include "dvs/test/ScopedTemporaryDirectory.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <string>
#include <vector>

namespace dvs::platform {
namespace {

[[nodiscard]] std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        lines.push_back(std::move(line));
    }
    return lines;
}

TEST(TraceSinkTests, FileSinkExportsIdentityEventsAndOverflowMarkersToUnicodePath) {
    test::ScopedTemporaryDirectory directory{"dvs-trace-sink"};
    const std::filesystem::path path = directory.path() / L"回放-trace.jsonl";
    {
        FileTraceSink sink{path};
        ASSERT_TRUE(sink.append(application::TraceEvent{
            .identity =
                application::TraceIdentity{
                    .session = domain::SessionId{1U},
                    .epoch = domain::SessionEpoch{2U},
                    .topology = domain::TopologyRevision{3U},
                    .timeline = domain::TimelineRevision{4U},
                    .alignment = domain::AlignmentRevision{5U},
                    .generation = domain::PlaybackGeneration{6U},
                    .device = domain::DeviceGeneration{7U},
                    .request = domain::RequestId{8U},
                    .command = domain::CommandId{9U},
                },
            .kind = application::TraceEventKind::CacheHit,
            .timestampMicroseconds = 10U,
            .payload = 11U,
        }));
        ASSERT_TRUE(sink.recordOverflow(12U));
        EXPECT_EQ(sink.overflowCount(), 12U);
        EXPECT_FALSE(std::filesystem::exists(path));
        ASSERT_TRUE(sink.finalize());
        EXPECT_TRUE(sink.finalize());
    }

    const std::vector<std::string> lines = readLines(path);
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_EQ(lines[0], R"({"traceVersion":1})");
    EXPECT_EQ(lines[1],
              R"({"t":10,"kind":12,"s":1,"e":2,"topo":3,"tl":4,)"
              R"("al":5,"gen":6,"dev":7,"req":8,"cmd":9,"p":11})");
    EXPECT_EQ(lines[2], R"({"overflow":12})");
}

TEST(TraceSinkTests, FileSinkExportsOptionalIncomingIdentityFields) {
    test::ScopedTemporaryDirectory directory{"dvs-trace-sink-incoming"};
    const std::filesystem::path path = directory.path() / L"incoming.jsonl";
    {
        FileTraceSink sink{path};
        ASSERT_TRUE(sink.append(application::TraceEvent{
            .identity =
                application::TraceIdentity{
                    .session = domain::SessionId{1U},
                    .epoch = domain::SessionEpoch{1U},
                    .topology = domain::TopologyRevision{1U},
                    .timeline = domain::TimelineRevision{1U},
                    .alignment = domain::AlignmentRevision{1U},
                    .generation = domain::PlaybackGeneration{2U},
                    .device = domain::DeviceGeneration{3U},
                    .request = domain::RequestId{4U},
                },
            .kind = application::TraceEventKind::FrameSetReady,
            .timestampMicroseconds = 5U,
            .payload = 6U,
            .incoming =
                application::TraceIncomingIdentity{
                    .session = domain::SessionId{1U},
                    .epoch = domain::SessionEpoch{1U},
                    .generation = domain::PlaybackGeneration{2U},
                    .device = domain::DeviceGeneration{3U},
                    .request = domain::RequestId{9U},
                },
        }));
        ASSERT_TRUE(sink.finalize());
    }

    const std::vector<std::string> lines = readLines(path);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], R"({"traceVersion":1})");
    EXPECT_EQ(
        lines[1],
        R"({"t":5,"kind":4,"s":1,"e":1,"topo":1,"tl":1,)"
        R"("al":1,"gen":2,"dev":3,"req":4,"cmd":null,"p":6,)"
        R"("is":1,"ie":1,"igen":2,"idev":3,"ireq":9})");
}

TEST(TraceSinkTests, MemorySinkRemainsBoundedAndAccumulatesOverflow) {
    MemoryTraceSink sink{1U};
    const application::TraceEvent event{
        .kind = application::TraceEventKind::CacheHit,
        .timestampMicroseconds = 1U,
    };
    EXPECT_TRUE(sink.append(event));
    EXPECT_FALSE(sink.append(event));
    EXPECT_TRUE(sink.recordOverflow(2U));
    EXPECT_TRUE(sink.recordOverflow(3U));
    EXPECT_TRUE(sink.finalize());

    EXPECT_EQ(sink.events().size(), 1U);
    EXPECT_EQ(sink.overflowCount(), 5U);

    sink.clear();
    EXPECT_TRUE(sink.events().empty());
    EXPECT_EQ(sink.overflowCount(), 0U);
}

TEST(TraceSinkTests, FileSinkDoesNotDropMaximumWidthSchemaValues) {
    test::ScopedTemporaryDirectory directory{"dvs-trace-width"};
    const std::filesystem::path path = directory.path() / "trace.jsonl";
    constexpr std::uint64_t kMaximum = std::numeric_limits<std::uint64_t>::max();
    {
        FileTraceSink sink{path};
        ASSERT_TRUE(sink.append(application::TraceEvent{
            .identity =
                application::TraceIdentity{
                    .session = domain::SessionId{kMaximum},
                    .epoch = domain::SessionEpoch{kMaximum},
                    .topology = domain::TopologyRevision{kMaximum},
                    .timeline = domain::TimelineRevision{kMaximum},
                    .alignment = domain::AlignmentRevision{kMaximum},
                    .generation = domain::PlaybackGeneration{kMaximum},
                    .device = domain::DeviceGeneration{kMaximum},
                    .request = domain::RequestId{kMaximum},
                    .command = domain::CommandId{kMaximum},
                },
            .kind = application::TraceEventKind::DeviceGenerationChanged,
            .timestampMicroseconds = kMaximum,
            .payload = kMaximum,
        }));
        ASSERT_TRUE(sink.finalize());
    }

    const std::vector<std::string> lines = readLines(path);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_GT(lines[1].size(), 256U);
    EXPECT_NE(lines[1].find(R"("t":18446744073709551615)"), std::string::npos);
    EXPECT_NE(lines[1].find(R"("cmd":18446744073709551615)"), std::string::npos);
    EXPECT_NE(lines[1].find(R"("p":18446744073709551615)"), std::string::npos);
}

TEST(TraceSinkTests, FileSinkPublishesTransactionallyAndReplacesAnExistingTrace) {
    test::ScopedTemporaryDirectory directory{"dvs-trace-transaction"};
    const std::filesystem::path path = directory.path() / "trace.jsonl";
    {
        std::ofstream existing{path, std::ios::binary};
        existing << "previous trace\n";
    }

    FileTraceSink sink{path};
    ASSERT_TRUE(sink.append(application::TraceEvent{
        .kind = application::TraceEventKind::CacheHit,
        .timestampMicroseconds = 1U,
    }));
    EXPECT_EQ(readLines(path), std::vector<std::string>{"previous trace"});
    ASSERT_TRUE(sink.finalize());

    const std::vector<std::string> lines = readLines(path);
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines.front(), R"({"traceVersion":1})");
}

TEST(TraceSinkTests, FileSinkAbandonsAnUnfinalizedPartialTrace) {
    test::ScopedTemporaryDirectory directory{"dvs-trace-abandon"};
    const std::filesystem::path path = directory.path() / "trace.jsonl";
    {
        FileTraceSink sink{path};
        ASSERT_TRUE(sink.append(application::TraceEvent{
            .kind = application::TraceEventKind::CacheHit,
            .timestampMicroseconds = 1U,
        }));
        EXPECT_FALSE(std::filesystem::exists(path));
    }

    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::is_empty(directory.path()));
}

} // namespace
} // namespace dvs::platform
