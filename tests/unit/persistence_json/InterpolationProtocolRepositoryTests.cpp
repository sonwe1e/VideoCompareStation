#include "dvs/persistence/InterpolationProtocolRepository.h"

#include "dvs/domain/InterpolationProtocol.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace dvs::persistence {
namespace {

namespace fs = std::filesystem;

using domain::DecimationPlan;
using domain::MappingOrigin;
using domain::TailPolicy;

class InterpolationProtocolRepositoryTests : public ::testing::Test {
protected:
    void SetUp() override {
        temp_ = fs::temp_directory_path() /
                ("dvs_interpolation_protocol_tests_" +
                 std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        fs::create_directories(temp_);
    }

    void TearDown() override {
        std::error_code errorCode;
        fs::remove_all(temp_, errorCode);
    }

    [[nodiscard]] fs::path documentPath() const {
        return temp_ / "protocol.json";
    }

    [[nodiscard]] fs::path writeDocument(const std::string& name, const std::string& text) const {
        const fs::path path = temp_ / name;
        std::ofstream stream{path, std::ios::binary};
        stream << text;
        return path;
    }

    fs::path temp_;
};

TEST_F(InterpolationProtocolRepositoryTests, ImportsDecimationPhaseZeroTrim) {
    const auto result = InterpolationProtocolRepository::decodeDocument(
        R"({"schema_version":1,"kind":"decimation","gt_frame_count":7,"stride":2,"phase":0,)"
        R"("tail_policy":"trim","mapping_origin":"imported_manifest"})");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.protocol.has_value());

    const auto expected = domain::buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 7},
                                                         MappingOrigin::kImportedManifest,
                                                         TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(expected);
    EXPECT_EQ(result.protocol.value(), expected.value());
}

TEST_F(InterpolationProtocolRepositoryTests, ImportsDecimationPhaseOneManualOrigin) {
    const auto result = InterpolationProtocolRepository::decodeDocument(
        R"({"schema_version":1,"kind":"decimation","gt_frame_count":8,"stride":2,"phase":1,)"
        R"("tail_policy":"trim","mapping_origin":"manual"})");
    ASSERT_TRUE(result.ok) << result.error;

    const auto expected = domain::buildDecimatedProtocol(
        DecimationPlan{.gtFrameCount = 8, .stride = 2, .phase = 1},
        MappingOrigin::kManual,
        TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(expected);
    EXPECT_EQ(result.protocol.value(), expected.value());
    EXPECT_EQ(result.protocol.value().mappingOrigin(), MappingOrigin::kManual);
}

TEST_F(InterpolationProtocolRepositoryTests, ImportsStrideThreeWithMultipleGeneratedMoments) {
    const auto result = InterpolationProtocolRepository::decodeDocument(
        R"({"schema_version":1,"kind":"decimation","gt_frame_count":10,"stride":3,"phase":0,)"
        R"("tail_policy":"trim"})");
    ASSERT_TRUE(result.ok) << result.error;
    // mapping_origin absent defaults to imported_manifest rather than guessing a manual entry.
    EXPECT_EQ(result.protocol.value().mappingOrigin(), MappingOrigin::kImportedManifest);

    const auto expected = domain::buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 10,
                                                                       .stride = 3},
                                                         MappingOrigin::kImportedManifest,
                                                         TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(expected);
    EXPECT_EQ(result.protocol.value(), expected.value());
    EXPECT_EQ(result.protocol.value().frameCount(), 10);
}

TEST_F(InterpolationProtocolRepositoryTests, ImportsMarkMissingTailPolicy) {
    const auto result = InterpolationProtocolRepository::decodeDocument(
        R"({"schema_version":1,"kind":"decimation","gt_frame_count":8,"stride":2,"phase":0,)"
        R"("tail_policy":"mark_missing","mapping_origin":"manual"})");
    ASSERT_TRUE(result.ok) << result.error;

    const auto expected = domain::buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 8},
                                                         MappingOrigin::kManual,
                                                         TailPolicy::kMarkPartialIntervalMissing);
    ASSERT_TRUE(expected);
    EXPECT_EQ(result.protocol.value(), expected.value());
    EXPECT_EQ(result.protocol.value().frameCount(), 8);
}

TEST_F(InterpolationProtocolRepositoryTests, ImportsExplicitOriginsWithAllFourKinds) {
    const auto result = InterpolationProtocolRepository::decodeDocument(R"({
        "schema_version": 1,
        "kind": "explicit",
        "gt_frame_count": 6,
        "origins": [
            {"type": "retained", "gt": 0},
            {"type": "generated", "input0": 0, "input1": 2, "ratio": [1, 2]},
            {"type": "unknown"},
            {"type": "missing", "expected": null},
            {"type": "generated", "input0": 2, "input1": 4, "ratio": [2, 3]},
            {"type": "missing", "expected": 4},
            {"type": "retained", "gt": 4}
        ]
    })");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.protocol.has_value());
    EXPECT_EQ(result.protocol.value().frameCount(), 7);
    EXPECT_EQ(result.protocol.value().gtFrameCount(), 6);
    EXPECT_EQ(result.protocol.value().mappingOrigin(), MappingOrigin::kImportedManifest);

    const auto& protocol = result.protocol.value();
    const auto retained = protocol.originAt(domain::FrameId{0});
    ASSERT_TRUE(retained);
    EXPECT_TRUE(std::holds_alternative<domain::RetainedFrame>(retained.value()));
    const auto generated = protocol.originAt(domain::FrameId{1});
    ASSERT_TRUE(generated);
    const auto* generatedFrame = std::get_if<domain::GeneratedFrame>(&generated.value());
    ASSERT_NE(generatedFrame, nullptr);
    EXPECT_EQ(generatedFrame->input0GtFrame, domain::GtFrameIndex{0});
    EXPECT_EQ(generatedFrame->input1GtFrame, domain::GtFrameIndex{2});
    EXPECT_EQ(generatedFrame->targetRatioNumerator, 1);
    EXPECT_EQ(generatedFrame->targetRatioDenominator, 2);
    EXPECT_TRUE(std::holds_alternative<domain::UnknownFrame>(
        protocol.originAt(domain::FrameId{2}).value()));
    const auto missingNoTarget = protocol.originAt(domain::FrameId{3});
    ASSERT_TRUE(missingNoTarget);
    const auto* missingFrame = std::get_if<domain::MissingFrame>(&missingNoTarget.value());
    ASSERT_NE(missingFrame, nullptr);
    EXPECT_FALSE(missingFrame->expectedGtFrame.has_value());
    const auto missingWithTarget = protocol.originAt(domain::FrameId{5});
    ASSERT_TRUE(missingWithTarget);
    const auto* missingExpected = std::get_if<domain::MissingFrame>(&missingWithTarget.value());
    ASSERT_NE(missingExpected, nullptr);
    ASSERT_TRUE(missingExpected->expectedGtFrame.has_value());
    EXPECT_EQ(*missingExpected->expectedGtFrame, domain::GtFrameIndex{4});
}

TEST_F(InterpolationProtocolRepositoryTests, ExplicitDocumentRoundTripsThroughEncoding) {
    const auto decoded = InterpolationProtocolRepository::decodeDocument(R"({
        "schema_version": 1,
        "kind": "explicit",
        "gt_frame_count": 6,
        "mapping_origin": "manual",
        "origins": [
            {"type": "retained", "gt": 0},
            {"type": "generated", "input0": 0, "input1": 2, "ratio": [2, 4]},
            {"type": "unknown"},
            {"type": "missing", "expected": null},
            {"type": "retained", "gt": 2},
            {"type": "missing", "expected": 3},
            {"type": "retained", "gt": 4}
        ]
    })");
    ASSERT_TRUE(decoded.ok) << decoded.error;

    const std::string encoded =
        InterpolationProtocolRepository::encodeDocument(decoded.protocol.value());
    const auto reparsed = InterpolationProtocolRepository::decodeDocument(encoded);
    ASSERT_TRUE(reparsed.ok) << reparsed.error;
    EXPECT_EQ(reparsed.protocol.value(), decoded.protocol.value());

    const auto third = InterpolationProtocolRepository::decodeDocument(
        InterpolationProtocolRepository::encodeDocument(reparsed.protocol.value()));
    ASSERT_TRUE(third.ok) << third.error;
    EXPECT_EQ(third.protocol.value(), decoded.protocol.value());
}

TEST_F(InterpolationProtocolRepositoryTests, EncodedDecimationProtocolReimportsIdentically) {
    // The explicit encoding is lossless for a protocol built from a decimation plan.
    const auto built = domain::buildDecimatedProtocol(DecimationPlan{.gtFrameCount = 10,
                                                                     .stride = 3},
                                                      MappingOrigin::kImportedManifest,
                                                      TailPolicy::kTrimPartialInterval);
    ASSERT_TRUE(built);
    const auto reparsed = InterpolationProtocolRepository::decodeDocument(
        InterpolationProtocolRepository::encodeDocument(built.value()));
    ASSERT_TRUE(reparsed.ok) << reparsed.error;
    EXPECT_EQ(reparsed.protocol.value(), built.value());
}

TEST_F(InterpolationProtocolRepositoryTests, RejectsInvalidJson) {
    const auto result = InterpolationProtocolRepository::decodeDocument("{\"schema_version\":1,");
    ASSERT_FALSE(result.ok);
    EXPECT_FALSE(result.protocol.has_value());
    EXPECT_NE(result.error.find("not valid UTF-8 JSON"), std::string::npos) << result.error;
}

TEST_F(InterpolationProtocolRepositoryTests, RejectsUnsupportedSchemaVersion) {
    const auto result = InterpolationProtocolRepository::decodeDocument(
        R"({"schema_version":2,"kind":"decimation","gt_frame_count":7,"stride":2,"phase":0,)"
        R"("tail_policy":"trim"})");
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("unsupported interpolation-protocol schema_version"),
              std::string::npos)
        << result.error;
}

TEST_F(InterpolationProtocolRepositoryTests, RejectsMissingSchemaVersion) {
    const auto result =
        InterpolationProtocolRepository::decodeDocument(R"({"kind":"explicit","origins":[]})");
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("requires an integer \"schema_version\""), std::string::npos)
        << result.error;
}

TEST_F(InterpolationProtocolRepositoryTests, RejectsUnknownKind) {
    const auto result = InterpolationProtocolRepository::decodeDocument(
        R"({"schema_version":1,"kind":"temporal","gt_frame_count":7})");
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("unknown interpolation-protocol kind \"temporal\""),
              std::string::npos)
        << result.error;
}

TEST_F(InterpolationProtocolRepositoryTests, RejectsDecimationWithMissingFields) {
    const auto missingStride = InterpolationProtocolRepository::decodeDocument(
        R"({"schema_version":1,"kind":"decimation","gt_frame_count":7,"phase":0,)"
        R"("tail_policy":"trim"})");
    ASSERT_FALSE(missingStride.ok);
    EXPECT_NE(missingStride.error.find("requires an integer \"stride\""), std::string::npos)
        << missingStride.error;

    const auto badTail = InterpolationProtocolRepository::decodeDocument(
        R"({"schema_version":1,"kind":"decimation","gt_frame_count":7,"stride":2,"phase":0,)"
        R"("tail_policy":"drop"})");
    ASSERT_FALSE(badTail.ok);
    EXPECT_NE(badTail.error.find("\"tail_policy\" must be \"trim\" or \"mark_missing\""),
              std::string::npos)
        << badTail.error;

    const auto badOrigin = InterpolationProtocolRepository::decodeDocument(
        R"({"schema_version":1,"kind":"decimation","gt_frame_count":7,"stride":2,"phase":0,)"
        R"("tail_policy":"trim","mapping_origin":"suggestion"})");
    ASSERT_FALSE(badOrigin.ok);
    EXPECT_NE(badOrigin.error.find("\"mapping_origin\" must be"), std::string::npos)
        << badOrigin.error;
}

TEST_F(InterpolationProtocolRepositoryTests, RejectsExplicitOriginsWithOutOfRangeIndices) {
    // gt 5 means valid indices are 0..4; a retained entry at 5 is out of range and the domain
    // create must reject the whole document.
    const auto result = InterpolationProtocolRepository::decodeDocument(R"({
        "schema_version": 1,
        "kind": "explicit",
        "gt_frame_count": 5,
        "origins": [{"type": "retained", "gt": 5}]
    })");
    ASSERT_FALSE(result.ok);
    EXPECT_NE(result.error.find("rejected by domain validation"), std::string::npos)
        << result.error;
    EXPECT_NE(result.error.find("exceeds the known GT frame count"), std::string::npos)
        << result.error;
}

TEST_F(InterpolationProtocolRepositoryTests, RejectsExplicitOriginsWithBadRatios) {
    const auto endpointRatio = InterpolationProtocolRepository::decodeDocument(R"({
        "schema_version": 1,
        "kind": "explicit",
        "gt_frame_count": 5,
        "origins": [{"type": "generated", "input0": 0, "input1": 2, "ratio": [2, 2]}]
    })");
    ASSERT_FALSE(endpointRatio.ok);
    EXPECT_NE(endpointRatio.error.find("rejected by domain validation"), std::string::npos)
        << endpointRatio.error;

    const auto missingRatio = InterpolationProtocolRepository::decodeDocument(R"({
        "schema_version": 1,
        "kind": "explicit",
        "gt_frame_count": 5,
        "origins": [{"type": "generated", "input0": 0, "input1": 2}]
    })");
    ASSERT_FALSE(missingRatio.ok);
    EXPECT_NE(missingRatio.error.find("requires a \"ratio\" pair of integers"),
              std::string::npos)
        << missingRatio.error;
}

TEST_F(InterpolationProtocolRepositoryTests, RejectsMalformedExplicitEntries) {
    const auto unknownType = InterpolationProtocolRepository::decodeDocument(R"({
        "schema_version": 1,
        "kind": "explicit",
        "gt_frame_count": 5,
        "origins": [{"type": "copied"}]
    })");
    ASSERT_FALSE(unknownType.ok);
    EXPECT_NE(unknownType.error.find("unknown \"type\": copied"), std::string::npos)
        << unknownType.error;

    const auto negativeIndex = InterpolationProtocolRepository::decodeDocument(R"({
        "schema_version": 1,
        "kind": "explicit",
        "gt_frame_count": 5,
        "origins": [{"type": "retained", "gt": -1}]
    })");
    ASSERT_FALSE(negativeIndex.ok);
    EXPECT_NE(negativeIndex.error.find("rejected by domain validation"), std::string::npos)
        << negativeIndex.error;

    const auto emptyOrigins = InterpolationProtocolRepository::decodeDocument(R"({
        "schema_version": 1,
        "kind": "explicit",
        "gt_frame_count": 5,
        "origins": []
    })");
    ASSERT_FALSE(emptyOrigins.ok);
    EXPECT_NE(emptyOrigins.error.find("rejected by domain validation"), std::string::npos)
        << emptyOrigins.error;

    const auto mistypedEntry = InterpolationProtocolRepository::decodeDocument(R"({
        "schema_version": 1,
        "kind": "explicit",
        "gt_frame_count": 5,
        "origins": [7]
    })");
    ASSERT_FALSE(mistypedEntry.ok);
    EXPECT_NE(mistypedEntry.error.find("origins[0] must be a JSON object"), std::string::npos)
        << mistypedEntry.error;
}

TEST_F(InterpolationProtocolRepositoryTests, LoadsDecimationDocumentFromFile) {
    const fs::path path =
        writeDocument("decimation.json",
                      R"({"schema_version":1,"kind":"decimation","gt_frame_count":10,)"
                      R"("stride":3,"phase":0,"tail_policy":"trim"})");
    const InterpolationProtocolRepository repository;
    const auto result = repository.load(path);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_TRUE(result.protocol.has_value());
    EXPECT_EQ(result.protocol.value().frameCount(), 10);
    EXPECT_EQ(result.protocol.value().nextGeneratedFrame(domain::FrameId{0}),
              domain::FrameId{1});
}

TEST_F(InterpolationProtocolRepositoryTests, ReportsFileLevelErrors) {
    const InterpolationProtocolRepository repository;

    const auto emptyPath = repository.load({});
    ASSERT_FALSE(emptyPath.ok);
    EXPECT_NE(emptyPath.error.find("path is empty"), std::string::npos) << emptyPath.error;

    const auto missing = repository.load(temp_ / "does-not-exist.json");
    ASSERT_FALSE(missing.ok);
    EXPECT_NE(missing.error.find("does not exist"), std::string::npos) << missing.error;

    const fs::path corrupt = writeDocument("corrupt.json", "not json at all");
    const auto parseFailure = repository.load(corrupt);
    ASSERT_FALSE(parseFailure.ok);
    EXPECT_NE(parseFailure.error.find("not valid UTF-8 JSON"), std::string::npos)
        << parseFailure.error;
}

} // namespace
} // namespace dvs::persistence
