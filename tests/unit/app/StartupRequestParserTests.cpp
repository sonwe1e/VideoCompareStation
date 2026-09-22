#include "StartupRequest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <filesystem>
#include <gtest/gtest.h>

namespace dvs::app {
namespace {

TEST(StartupRequestParserTests, ParsesEmptyLaunch) {
    const StartupRequestParseResult result = parseStartupRequest({QStringLiteral("CompareStation.exe")});

    ASSERT_TRUE(result);
    EXPECT_EQ(result.request->kind, StartupRequest::Kind::Empty);
    EXPECT_TRUE(result.request->sources.empty());
}

TEST(StartupRequestParserTests, ParsesUnicodeSinglePlaybackWithoutNarrowing) {
    const StartupRequestParseResult result =
        parseStartupRequest({QStringLiteral("CompareStation.exe"),
                             QStringLiteral("--play"),
                             QStringLiteral(R"(C:\视频 工作区\源 甲.mp4)")});

    ASSERT_TRUE(result);
    EXPECT_EQ(result.request->kind, StartupRequest::Kind::PlaySingle);
    ASSERT_EQ(result.request->sources.size(), 1U);
    EXPECT_EQ(result.request->sources.front(),
              std::filesystem::path{LR"(C:\视频 工作区\源 甲.mp4)"});
}

TEST(StartupRequestParserTests, ParsesTwoAndThreeSourceComparison) {
    const auto two = parseStartupRequest({QStringLiteral("CompareStation.exe"),
                                          QStringLiteral("--compare"),
                                          QStringLiteral(R"(C:\a one.mp4)"),
                                          QStringLiteral(R"(D:\b two.mkv)")});
    const auto three = parseStartupRequest({QStringLiteral("CompareStation.exe"),
                                            QStringLiteral("--compare"),
                                            QStringLiteral(R"(C:\a.mp4)"),
                                            QStringLiteral(R"(C:\b.mp4)"),
                                            QStringLiteral(R"(C:\c.mp4)")});

    ASSERT_TRUE(two);
    EXPECT_EQ(two.request->kind, StartupRequest::Kind::Compare);
    EXPECT_EQ(two.request->sources.size(), 2U);
    ASSERT_TRUE(three);
    EXPECT_EQ(three.request->kind, StartupRequest::Kind::Compare);
    EXPECT_EQ(three.request->sources.size(), 3U);
}

TEST(StartupRequestParserTests, ParsesOneTwoAndThreeBareVideoPaths) {
    const auto one = parseStartupRequest(
        {QStringLiteral("CompareStation.exe"), QStringLiteral(R"(C:\one video.mp4)")});
    const auto two = parseStartupRequest({QStringLiteral("CompareStation.exe"),
                                          QStringLiteral(R"(C:\one.mp4)"),
                                          QStringLiteral(R"(D:\two.mkv)")});
    const auto three = parseStartupRequest({QStringLiteral("CompareStation.exe"),
                                            QStringLiteral(R"(C:\one.mp4)"),
                                            QStringLiteral(R"(D:\two.mkv)"),
                                            QStringLiteral(R"(E:\three.mov)")});

    ASSERT_TRUE(one);
    EXPECT_EQ(one.request->kind, StartupRequest::Kind::PlaySingle);
    ASSERT_TRUE(two);
    EXPECT_EQ(two.request->kind, StartupRequest::Kind::Compare);
    EXPECT_EQ(two.request->sources.size(), 2U);
    ASSERT_TRUE(three);
    EXPECT_EQ(three.request->kind, StartupRequest::Kind::Compare);
    EXPECT_EQ(three.request->sources.size(), 3U);
}

TEST(StartupRequestParserTests, RejectsAmbiguousOrOutOfRangeArguments) {
    EXPECT_FALSE(parseStartupRequest(
        {QStringLiteral("CompareStation.exe"), QStringLiteral("--compare"), QStringLiteral("one.mp4")}));
    EXPECT_FALSE(parseStartupRequest({QStringLiteral("CompareStation.exe"),
                                      QStringLiteral("--play"),
                                      QStringLiteral("one.mp4"),
                                      QStringLiteral("two.mp4")}));
    EXPECT_FALSE(parseStartupRequest({QStringLiteral("CompareStation.exe"),
                                      QStringLiteral("one.mp4"),
                                      QStringLiteral("two.mp4"),
                                      QStringLiteral("three.mp4"),
                                      QStringLiteral("four.mp4")}));
    EXPECT_FALSE(
        parseStartupRequest({QStringLiteral("CompareStation.exe"), QStringLiteral("--unknown")}));
}

TEST(StartupRequestParserTests, BoundedJsonRoundTripPreservesUnicodePaths) {
    const StartupRequest original{
        .kind = StartupRequest::Kind::Compare,
        .sources =
            {
                std::filesystem::path{LR"(C:\素材\一号 源.mp4)"},
                std::filesystem::path{LR"(D:\素材\二号 源.mkv)"},
            },
    };

    const QByteArray encoded = encodeStartupRequest(original);
    const StartupRequestParseResult decoded = decodeStartupRequest(encoded);

    ASSERT_FALSE(encoded.isEmpty());
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded.request, original);
}

TEST(StartupRequestParserTests, RejectsOversizedOrUnknownJson) {
    EXPECT_FALSE(decodeStartupRequest(QByteArray{65 * 1024, 'x'}));

    const QByteArray unknown = QJsonDocument{
        QJsonObject{
            {QStringLiteral("version"), 1},
            {QStringLiteral("kind"), QStringLiteral("launch-everything")},
            {QStringLiteral("sources"), QJsonArray{}},
        }}.toJson(QJsonDocument::Compact);
    EXPECT_FALSE(decodeStartupRequest(unknown));
}

} // namespace
} // namespace dvs::app
