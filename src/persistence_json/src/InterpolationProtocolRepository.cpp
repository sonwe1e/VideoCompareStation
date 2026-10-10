#include "dvs/persistence/InterpolationProtocolRepository.h"

#include "dvs/domain/InterpolationProtocol.h"
#include "dvs/domain/MediaError.h"

#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dvs::persistence {
namespace {

using Json = nlohmann::json;
using application::InterpolationProtocolIoResult;
using domain::GeneratedFrame;
using domain::GtFrameIndex;
using domain::InterpolationProtocol;
using domain::MappingOrigin;
using domain::MissingFrame;
using domain::OutputFrameOrigin;
using domain::RetainedFrame;
using domain::UnknownFrame;

// Decimation manifests are tiny; explicit per-frame manifests are the large ones. 16 MiB covers
// an hour of 60 fps with one entry per output position while still bounding a hostile file.
constexpr std::uintmax_t kMaximumProtocolDocumentBytes = std::uintmax_t{16U} * 1024U * 1024U;

[[nodiscard]] InterpolationProtocolIoResult failure(std::string error) {
    InterpolationProtocolIoResult result;
    result.error = std::move(error);
    return result;
}

// Required integer field: unlike the defensive issue-record readers (tolerant of older files), a
// protocol manifest is machine-written, so a missing or mistyped field is a hard error naming
// the field. is_number_integer() accepts signed and unsigned JSON numbers.
[[nodiscard]] bool readRequiredInteger(const Json& value,
                                       const char* const key,
                                       std::int64_t* out,
                                       std::string* error) {
    const auto field = value.find(key);
    if (field == value.end() || !field->is_number_integer()) {
        *error = std::string{"interpolation-protocol document requires an integer \""} + key +
                 "\".";
        return false;
    }
    *out = field->get<std::int64_t>();
    return true;
}

[[nodiscard]] bool readMappingOrigin(const Json& root, MappingOrigin* out, std::string* error) {
    const auto field = root.find("mapping_origin");
    if (field == root.end()) {
        // Absent means the document did not record one; an imported file defaults to the
        // manifest origin rather than guessing a manual entry.
        *out = MappingOrigin::kImportedManifest;
        return true;
    }
    if (!field->is_string()) {
        *error = "interpolation-protocol \"mapping_origin\" must be a string.";
        return false;
    }
    const std::string text = field->get<std::string>();
    if (text == "imported_manifest") {
        *out = MappingOrigin::kImportedManifest;
        return true;
    }
    if (text == "manual") {
        *out = MappingOrigin::kManual;
        return true;
    }
    *error = "interpolation-protocol \"mapping_origin\" must be \"imported_manifest\" or "
             "\"manual\"; \"suggestion\" mappings are unconfirmed in-app state and cannot be "
             "imported.";
    return false;
}

[[nodiscard]] InterpolationProtocolIoResult
domainFailure(const domain::MediaError& error) {
    return failure("interpolation-protocol rejected by domain validation: " +
                   error.technicalDetail);
}

[[nodiscard]] InterpolationProtocolIoResult decodeDecimation(const Json& root) {
    std::int64_t gtFrameCount = 0;
    std::int64_t stride = 0;
    std::int64_t phase = 0;
    std::string error;
    if (!readRequiredInteger(root, "gt_frame_count", &gtFrameCount, &error) ||
        !readRequiredInteger(root, "stride", &stride, &error) ||
        !readRequiredInteger(root, "phase", &phase, &error)) {
        return failure(std::move(error));
    }
    const auto tailPolicyField = root.find("tail_policy");
    if (tailPolicyField == root.end() || !tailPolicyField->is_string()) {
        return failure(
            "interpolation-protocol decimation requires a \"tail_policy\" string (\"trim\" or "
            "\"mark_missing\").");
    }
    const std::string tailPolicyText = tailPolicyField->get<std::string>();
    if (tailPolicyText != "trim" && tailPolicyText != "mark_missing") {
        return failure("interpolation-protocol \"tail_policy\" must be \"trim\" or "
                       "\"mark_missing\".");
    }
    const domain::TailPolicy tailPolicy = tailPolicyText == "trim"
                                               ? domain::TailPolicy::kTrimPartialInterval
                                               : domain::TailPolicy::kMarkPartialIntervalMissing;
    MappingOrigin mappingOrigin = MappingOrigin::kImportedManifest;
    if (!readMappingOrigin(root, &mappingOrigin, &error)) {
        return failure(std::move(error));
    }
    const auto built = domain::buildDecimatedProtocol(
        domain::DecimationPlan{
            .gtFrameCount = gtFrameCount,
            .stride = stride,
            .phase = phase,
        },
        mappingOrigin,
        tailPolicy);
    if (!built) {
        return domainFailure(built.error());
    }
    InterpolationProtocolIoResult result;
    result.ok = true;
    result.protocol = std::move(built).value();
    return result;
}

[[nodiscard]] bool parseOrigin(const Json& value,
                               const std::size_t index,
                               OutputFrameOrigin* out,
                               std::string* error) {
    if (!value.is_object()) {
        *error = "interpolation-protocol origins[" + std::to_string(index) +
                 "] must be a JSON object.";
        return false;
    }
    const auto typeField = value.find("type");
    if (typeField == value.end() || !typeField->is_string()) {
        *error = "interpolation-protocol origins[" + std::to_string(index) +
                 "] requires a \"type\" string.";
        return false;
    }
    const std::string type = typeField->get<std::string>();
    const auto prefixError = [error, index] {
        *error = "interpolation-protocol origins[" + std::to_string(index) + "]: " + *error;
    };
    if (type == "retained") {
        std::int64_t gt = 0;
        if (!readRequiredInteger(value, "gt", &gt, error)) {
            prefixError();
            return false;
        }
        *out = RetainedFrame{GtFrameIndex{gt}};
        return true;
    }
    if (type == "generated") {
        std::int64_t input0 = 0;
        std::int64_t input1 = 0;
        std::int64_t numerator = 0;
        std::int64_t denominator = 0;
        if (!readRequiredInteger(value, "input0", &input0, error) ||
            !readRequiredInteger(value, "input1", &input1, error)) {
            prefixError();
            return false;
        }
        const auto ratioField = value.find("ratio");
        if (ratioField == value.end() || !ratioField->is_array() ||
            ratioField->size() != 2U || !(*ratioField)[0].is_number_integer() ||
            !(*ratioField)[1].is_number_integer()) {
            *error = "interpolation-protocol origins[" + std::to_string(index) +
                     "] requires a \"ratio\" pair of integers [numerator, denominator].";
            return false;
        }
        numerator = (*ratioField)[0].get<std::int64_t>();
        denominator = (*ratioField)[1].get<std::int64_t>();
        *out = GeneratedFrame{GtFrameIndex{input0}, GtFrameIndex{input1}, numerator, denominator};
        return true;
    }
    if (type == "unknown") {
        *out = UnknownFrame{};
        return true;
    }
    if (type == "missing") {
        const auto expectedField = value.find("expected");
        if (expectedField == value.end() || expectedField->is_null()) {
            *out = MissingFrame{};
            return true;
        }
        if (!expectedField->is_number_integer()) {
            *error = "interpolation-protocol origins[" + std::to_string(index) +
                     "] \"expected\" must be an integer or null.";
            return false;
        }
        *out = MissingFrame{GtFrameIndex{expectedField->get<std::int64_t>()}};
        return true;
    }
    *error = "interpolation-protocol origins[" + std::to_string(index) +
             "] has unknown \"type\": " + type + ".";
    return false;
}

[[nodiscard]] InterpolationProtocolIoResult decodeExplicit(const Json& root) {
    std::int64_t gtFrameCount = 0;
    std::string error;
    if (!readRequiredInteger(root, "gt_frame_count", &gtFrameCount, &error)) {
        return failure(std::move(error));
    }
    MappingOrigin mappingOrigin = MappingOrigin::kImportedManifest;
    if (!readMappingOrigin(root, &mappingOrigin, &error)) {
        return failure(std::move(error));
    }
    const auto originsField = root.find("origins");
    if (originsField == root.end() || !originsField->is_array()) {
        return failure("interpolation-protocol explicit document requires an \"origins\" array.");
    }
    std::vector<OutputFrameOrigin> origins;
    origins.reserve(originsField->size());
    for (const Json& value : *originsField) {
        OutputFrameOrigin origin;
        if (!parseOrigin(value, origins.size(), &origin, &error)) {
            return failure(std::move(error));
        }
        origins.push_back(origin);
    }
    const auto created = InterpolationProtocol::create(gtFrameCount, mappingOrigin, origins);
    if (!created) {
        return domainFailure(created.error());
    }
    InterpolationProtocolIoResult result;
    result.ok = true;
    result.protocol = std::move(created).value();
    return result;
}

[[nodiscard]] std::string mappingOriginToString(const MappingOrigin origin) {
    switch (origin) {
    case MappingOrigin::kImportedManifest:
        return "imported_manifest";
    case MappingOrigin::kManual:
        return "manual";
    case MappingOrigin::kSuggestion:
        // Suggestions are unconfirmed in-app state; serializing the tag keeps the file honest
        // (and un-importable) instead of silently promoting it to a manual mapping.
        return "suggestion";
    }
    return "suggestion";
}

[[nodiscard]] Json originToJson(const OutputFrameOrigin& origin) {
    Json value = Json::object();
    if (const auto* retained = std::get_if<RetainedFrame>(&origin)) {
        value["type"] = "retained";
        value["gt"] = retained->gtFrame.value();
        return value;
    }
    if (const auto* generated = std::get_if<GeneratedFrame>(&origin)) {
        value["type"] = "generated";
        value["input0"] = generated->input0GtFrame.value();
        value["input1"] = generated->input1GtFrame.value();
        value["ratio"] = Json::array(
            {generated->targetRatioNumerator, generated->targetRatioDenominator});
        return value;
    }
    if (const auto* missing = std::get_if<MissingFrame>(&origin)) {
        value["type"] = "missing";
        // Parenthesized construction: brace-initializing a scalar Json would build a one-element
        // array instead of a number.
        if (missing->expectedGtFrame.has_value()) {
            value["expected"] = Json(missing->expectedGtFrame->value());
        } else {
            value["expected"] = Json();
        }
        return value;
    }
    value["type"] = "unknown";
    return value;
}

} // namespace

InterpolationProtocolIoResult
InterpolationProtocolRepository::decodeDocument(const std::string& text) {
    try {
        const Json root = Json::parse(text);
        if (!root.is_object()) {
            return failure("interpolation-protocol document must be a JSON object.");
        }
        const auto version = root.find("schema_version");
        if (version == root.end() || !version->is_number_integer()) {
            return failure("interpolation-protocol document requires an integer "
                           "\"schema_version\".");
        }
        const std::int64_t schemaVersion = version->get<std::int64_t>();
        if (schemaVersion != application::kInterpolationProtocolSchemaVersion) {
            // Explicit rejection: never treat an older/newer schema as silently compatible.
            return failure("unsupported interpolation-protocol schema_version (expected " +
                           std::to_string(application::kInterpolationProtocolSchemaVersion) +
                           ", got " + std::to_string(schemaVersion) + ").");
        }
        const auto kind = root.find("kind");
        if (kind == root.end() || !kind->is_string()) {
            return failure("interpolation-protocol document requires a \"kind\" string "
                           "(\"decimation\" or \"explicit\").");
        }
        const std::string kindText = kind->get<std::string>();
        if (kindText == "decimation") {
            return decodeDecimation(root);
        }
        if (kindText == "explicit") {
            return decodeExplicit(root);
        }
        return failure("unknown interpolation-protocol kind \"" + kindText +
                       "\" (expected \"decimation\" or \"explicit\").");
    } catch (const std::exception&) {
        return failure("interpolation-protocol document is not valid UTF-8 JSON.");
    }
}

std::string
InterpolationProtocolRepository::encodeDocument(const domain::InterpolationProtocol& protocol) {
    Json origins = Json::array();
    for (std::int64_t index = 0; index < protocol.frameCount(); ++index) {
        const auto origin = protocol.originAt(domain::FrameId{index});
        if (!origin) {
            // Unreachable for an index inside frameCount(); kept defensive so serialization
            // never invents provenance.
            origins.push_back(Json{{"type", "unknown"}});
            continue;
        }
        origins.push_back(originToJson(origin.value()));
    }
    Json document = Json::object();
    document["schema_version"] = application::kInterpolationProtocolSchemaVersion;
    document["kind"] = "explicit";
    document["gt_frame_count"] = protocol.gtFrameCount();
    document["mapping_origin"] = mappingOriginToString(protocol.mappingOrigin());
    document["origins"] = std::move(origins);
    std::string text = document.dump(2);
    text.push_back('\n');
    return text;
}

InterpolationProtocolIoResult
InterpolationProtocolRepository::load(const std::filesystem::path& path) const {
    if (path.empty()) {
        return failure("interpolation-protocol path is empty.");
    }
    std::error_code errorCode;
    const bool exists = std::filesystem::exists(path, errorCode);
    if (errorCode) {
        return failure("could not inspect the interpolation-protocol file.");
    }
    if (!exists) {
        return failure("interpolation-protocol file does not exist.");
    }
    const std::uintmax_t byteCount = std::filesystem::file_size(path, errorCode);
    if (errorCode || byteCount > kMaximumProtocolDocumentBytes) {
        return failure("interpolation-protocol file is unavailable or exceeds the 16 MiB "
                       "document limit.");
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return failure("could not open the interpolation-protocol file for reading.");
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    // Transactional by construction: the document is fully read, parsed, and validated before
    // any protocol is returned; a failure at any stage yields ok == false with no partial state.
    return decodeDocument(buffer.str());
}

} // namespace dvs::persistence
