#include "dvs/persistence/IssueRecordRepository.h"

#include "dvs/platform/AtomicFilePublisher.h"
#include "dvs/platform/WindowsPaths.h"

#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <span>
#include <sstream>
#include <utility>

namespace dvs::persistence {
namespace {

using Json = nlohmann::json;
using application::IssueRecord;
using application::IssueRecordKind;
using application::kIssueRecordSchemaVersion;

constexpr std::uintmax_t kMaximumIssueDocumentBytes = 2U * 1024U * 1024U;

[[nodiscard]] bool isSupportedSchemaVersion(const Json& value) {
    if (!value.is_number_integer() && !value.is_number_unsigned()) {
        return false;
    }
    const std::int64_t version = value.is_number_integer()
                                     ? value.get<std::int64_t>()
                                     : static_cast<std::int64_t>(value.get<std::uint64_t>());
    return version == kIssueRecordSchemaVersion;
}

[[nodiscard]] std::string kindToString(const IssueRecordKind kind) {
    return kind == IssueRecordKind::ImagePair ? "image-pair" : "video";
}

[[nodiscard]] Json sourceToJson(const application::IssueSourceRef& source) {
    Json value = Json::object();
    value["path"] = source.path;
    value["byteSize"] = source.byteSize;
    value["modifiedUtcMilliseconds"] = source.modifiedUtcMilliseconds;
    if (!source.fingerprintSha256.empty()) {
        value["fingerprintSha256"] = source.fingerprintSha256;
    }
    value["hasPresentation"] = source.hasPresentation;
    value["displayIndex"] = source.displayIndex;
    value["presentationTimestampTicks"] = source.presentationTimestampTicks;
    value["timeBaseNumerator"] = source.timeBaseNumerator;
    value["timeBaseDenominator"] = source.timeBaseDenominator;
    value["presentationMatchKind"] = source.presentationMatchKind;
    value["presentationMissingReason"] = source.presentationMissingReason;
    return value;
}

[[nodiscard]] application::IssueSourceRef sourceFromJson(const Json& value) {
    application::IssueSourceRef source;
    if (value.contains("path") && value["path"].is_string()) {
        source.path = value["path"].get<std::string>();
    }
    if (value.contains("byteSize") && value["byteSize"].is_number_integer()) {
        source.byteSize = value["byteSize"].get<std::int64_t>();
    }
    if (value.contains("modifiedUtcMilliseconds") &&
        value["modifiedUtcMilliseconds"].is_number_integer()) {
        source.modifiedUtcMilliseconds = value["modifiedUtcMilliseconds"].get<std::int64_t>();
    }
    if (value.contains("fingerprintSha256") && value["fingerprintSha256"].is_string()) {
        source.fingerprintSha256 = value["fingerprintSha256"].get<std::string>();
    }
    if (value.contains("hasPresentation") && value["hasPresentation"].is_boolean()) {
        source.hasPresentation = value["hasPresentation"].get<bool>();
    }
    if (value.contains("displayIndex") && value["displayIndex"].is_number_integer()) {
        source.displayIndex = value["displayIndex"].get<std::int64_t>();
    }
    if (value.contains("presentationTimestampTicks") &&
        value["presentationTimestampTicks"].is_number_integer()) {
        source.presentationTimestampTicks = value["presentationTimestampTicks"].get<std::int64_t>();
    }
    if (value.contains("timeBaseNumerator") && value["timeBaseNumerator"].is_number_integer()) {
        source.timeBaseNumerator = value["timeBaseNumerator"].get<std::int32_t>();
    }
    if (value.contains("timeBaseDenominator") && value["timeBaseDenominator"].is_number_integer()) {
        source.timeBaseDenominator = value["timeBaseDenominator"].get<std::int32_t>();
    }
    if (value.contains("presentationMatchKind") &&
        value["presentationMatchKind"].is_number_integer()) {
        source.presentationMatchKind = value["presentationMatchKind"].get<std::int32_t>();
    }
    if (value.contains("presentationMissingReason") &&
        value["presentationMissingReason"].is_number_integer()) {
        source.presentationMissingReason = value["presentationMissingReason"].get<std::int32_t>();
    }
    return source;
}

[[nodiscard]] Json recordToJson(const IssueRecord& record) {
    Json value = Json::object();
    value["schemaVersion"] = record.schemaVersion;
    value["kind"] = kindToString(record.kind);
    value["createdAtUtcMilliseconds"] = record.createdAtUtcMilliseconds;
    value["note"] = record.note;
    value["screenshot"] = Json{
        {"kind", "display-result"},
        {"path", record.screenshotPath},
    };
    value["hasValidPresentation"] = record.hasValidPresentation;
    value["canonicalSourceIndex"] = record.canonicalSourceIndex;
    value["alignmentRevision"] = record.alignmentRevision;
    value["view"] = Json{
        {"viewMode", record.view.viewMode},
        {"differenceEdge", record.view.differenceEdge},
        {"roiEnabled", record.view.roiEnabled},
        {"roiLeft", record.view.roiLeft},
        {"roiTop", record.view.roiTop},
        {"roiRight", record.view.roiRight},
        {"roiBottom", record.view.roiBottom},
        {"zoom", record.view.zoom},
        {"centerX", record.view.centerX},
        {"centerY", record.view.centerY},
        {"compareMode", record.view.compareMode},
        {"panX", record.view.panX},
        {"panY", record.view.panY},
        {"resampleAllowed", record.view.resampleAllowed},
    };
    Json sources = Json::array();
    for (const application::IssueSourceRef& source : record.sources) {
        sources.push_back(sourceToJson(source));
    }
    value["sources"] = std::move(sources);
    if (record.kind == IssueRecordKind::ImagePair) {
        value["imagePair"] = Json{
            {"leftFolder", record.leftFolder},
            {"rightFolder", record.rightFolder},
            {"leftPath", record.leftPath},
            {"rightPath", record.rightPath},
            {"fileName", record.fileName},
            {"rowIndex", record.rowIndex},
        };
    }
    return value;
}

[[nodiscard]] IssueRecord recordFromJson(const Json& value) {
    IssueRecord record;
    if (value.contains("schemaVersion") && value["schemaVersion"].is_number_integer()) {
        record.schemaVersion = value["schemaVersion"].get<std::int64_t>();
    }
    if (value.contains("kind") && value["kind"].is_string()) {
        const std::string kind = value["kind"].get<std::string>();
        record.kind = kind == "image-pair" ? IssueRecordKind::ImagePair : IssueRecordKind::Video;
    }
    if (value.contains("createdAtUtcMilliseconds") &&
        value["createdAtUtcMilliseconds"].is_number_integer()) {
        record.createdAtUtcMilliseconds = value["createdAtUtcMilliseconds"].get<std::int64_t>();
    }
    if (value.contains("note") && value["note"].is_string()) {
        record.note = value["note"].get<std::string>();
    }
    if (value.contains("screenshot") && value["screenshot"].is_object()) {
        const Json& screenshot = value["screenshot"];
        record.screenshotIsDisplayResult = true;
        if (screenshot.contains("path") && screenshot["path"].is_string()) {
            record.screenshotPath = screenshot["path"].get<std::string>();
        }
        if (screenshot.contains("kind") && screenshot["kind"].is_string() &&
            screenshot["kind"].get<std::string>() != "display-result") {
            record.screenshotIsDisplayResult = false;
        }
    }
    if (value.contains("hasValidPresentation") && value["hasValidPresentation"].is_boolean()) {
        record.hasValidPresentation = value["hasValidPresentation"].get<bool>();
    }
    if (value.contains("canonicalSourceIndex") &&
        value["canonicalSourceIndex"].is_number_integer()) {
        record.canonicalSourceIndex = value["canonicalSourceIndex"].get<std::int32_t>();
    }
    if (value.contains("alignmentRevision") && value["alignmentRevision"].is_number_unsigned()) {
        record.alignmentRevision = value["alignmentRevision"].get<std::uint64_t>();
    } else if (value.contains("alignmentRevision") &&
               value["alignmentRevision"].is_number_integer()) {
        const auto signedRevision = value["alignmentRevision"].get<std::int64_t>();
        record.alignmentRevision =
            signedRevision > 0 ? static_cast<std::uint64_t>(signedRevision) : 0U;
    }
    if (value.contains("view") && value["view"].is_object()) {
        const Json& view = value["view"];
        auto readNumber = [&view](const char* key, double fallback) {
            if (!view.contains(key) || !view[key].is_number()) {
                return fallback;
            }
            return view[key].get<double>();
        };
        auto readInteger = [&view](const char* key, std::int32_t fallback) {
            if (!view.contains(key) || !view[key].is_number_integer()) {
                return fallback;
            }
            return view[key].get<std::int32_t>();
        };
        record.view.viewMode = readInteger("viewMode", 0);
        record.view.differenceEdge = readInteger("differenceEdge", 0);
        if (view.contains("roiEnabled") && view["roiEnabled"].is_boolean()) {
            record.view.roiEnabled = view["roiEnabled"].get<bool>();
        }
        record.view.roiLeft = readNumber("roiLeft", 0.0);
        record.view.roiTop = readNumber("roiTop", 0.0);
        record.view.roiRight = readNumber("roiRight", 1.0);
        record.view.roiBottom = readNumber("roiBottom", 1.0);
        record.view.zoom = readNumber("zoom", 1.0);
        record.view.centerX = readNumber("centerX", 0.5);
        record.view.centerY = readNumber("centerY", 0.5);
        record.view.compareMode = readInteger("compareMode", 0);
        record.view.panX = readNumber("panX", 0.5);
        record.view.panY = readNumber("panY", 0.5);
        if (view.contains("resampleAllowed") && view["resampleAllowed"].is_boolean()) {
            record.view.resampleAllowed = view["resampleAllowed"].get<bool>();
        }
    }
    if (value.contains("sources") && value["sources"].is_array()) {
        for (const Json& sourceValue : value["sources"]) {
            if (sourceValue.is_object()) {
                record.sources.push_back(sourceFromJson(sourceValue));
            }
        }
    }
    if (value.contains("imagePair") && value["imagePair"].is_object()) {
        const Json& pair = value["imagePair"];
        auto readString = [&pair](const char* key) {
            return (pair.contains(key) && pair[key].is_string()) ? pair[key].get<std::string>()
                                                                 : std::string{};
        };
        record.leftFolder = readString("leftFolder");
        record.rightFolder = readString("rightFolder");
        record.leftPath = readString("leftPath");
        record.rightPath = readString("rightPath");
        record.fileName = readString("fileName");
        if (pair.contains("rowIndex") && pair["rowIndex"].is_number_integer()) {
            record.rowIndex = pair["rowIndex"].get<std::int32_t>();
        }
    }
    return record;
}

[[nodiscard]] std::filesystem::path defaultDocumentPath() {
    const auto applicationPaths = platform::WindowsPaths::applicationDataPaths();
    if (!applicationPaths) {
        return std::filesystem::path{};
    }
    return applicationPaths.value().userDataDirectory / "issue-records" / "session-issues.json";
}

} // namespace

IssueRecordRepository::IssueRecordRepository(std::filesystem::path documentPath)
    : documentPath_(documentPath.empty() ? defaultDocumentPath() : std::move(documentPath)) {}

IssueRecordRepository::~IssueRecordRepository() = default;

const std::filesystem::path& IssueRecordRepository::documentPath() const noexcept {
    return documentPath_;
}

std::string IssueRecordRepository::encodeDocument(const std::vector<IssueRecord>& records) {
    Json recordsArray = Json::array();
    for (const IssueRecord& record : records) {
        recordsArray.push_back(recordToJson(record));
    }
    Json document = Json::object();
    document["schemaVersion"] = kIssueRecordSchemaVersion;
    document["kind"] = "comparestation-issue-log";
    document["records"] = std::move(recordsArray);
    std::string text = document.dump(2);
    text.push_back('\n');
    return text;
}

IssueRecordDocument IssueRecordRepository::decodeDocument(const std::string& text) {
    IssueRecordDocument document;
    try {
        const Json root = Json::parse(text);
        if (!root.is_object()) {
            document.error = "issue-record document must be a JSON object.";
            return document;
        }
        const auto version = root.find("schemaVersion");
        if (version == root.end() || !isSupportedSchemaVersion(*version)) {
            // Explicit rejection: never treat an older/newer schema as silently compatible.
            document.error =
                "unsupported issue-record schemaVersion (expected 1); the file was left unchanged";
            return document;
        }
        const auto records = root.find("records");
        if (records == root.end() || !records->is_array()) {
            document.error = "issue-record document must contain a records array.";
            return document;
        }
        for (const Json& value : *records) {
            if (!value.is_object()) {
                document.error = "issue-record entries must be JSON objects.";
                document.records.clear();
                return document;
            }
            IssueRecord record = recordFromJson(value);
            if (record.schemaVersion != kIssueRecordSchemaVersion) {
                document.error = "unsupported issue-record schemaVersion (expected 1); the file "
                                 "was left unchanged";
                document.records.clear();
                return document;
            }
            document.records.push_back(std::move(record));
        }
        document.ok = true;
        return document;
    } catch (const std::exception&) {
        document.error = "issue-record document is not valid UTF-8 JSON.";
        return document;
    }
}

IssueRecordDocument IssueRecordRepository::loadDocument() const {
    return loadDocumentFrom(documentPath_);
}

IssueRecordDocument
IssueRecordRepository::loadDocumentFrom(const std::filesystem::path& path) const {
    IssueRecordDocument document;
    if (path.empty()) {
        document.error = "issue-record path is empty.";
        return document;
    }
    std::error_code errorCode;
    const bool exists = std::filesystem::exists(path, errorCode);
    if (errorCode) {
        document.error = "could not inspect the issue-record file.";
        return document;
    }
    if (!exists) {
        document.ok = true;
        document.records = {};
        return document;
    }
    const std::uintmax_t byteCount = std::filesystem::file_size(path, errorCode);
    if (errorCode || byteCount > kMaximumIssueDocumentBytes) {
        document.error = "issue-record file is unavailable or exceeds the 2 MiB document limit.";
        return document;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        document.error = "could not open the issue-record file for reading.";
        return document;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return decodeDocument(buffer.str());
}

application::IssueRecordIoResult
IssueRecordRepository::load(const std::filesystem::path& path) const {
    const IssueRecordDocument document = loadDocumentFrom(path.empty() ? documentPath_ : path);
    application::IssueRecordIoResult result;
    result.ok = document.ok;
    result.error = document.error;
    result.records = document.records;
    return result;
}

bool IssueRecordRepository::saveDocument(const std::vector<IssueRecord>& records,
                                         std::string* error) const {
    return save(documentPath_, records, error);
}

bool IssueRecordRepository::save(const std::filesystem::path& path,
                                 const std::vector<IssueRecord>& records,
                                 std::string* error) const {
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };
    const std::filesystem::path target = path.empty() ? documentPath_ : path;
    if (target.empty()) {
        return fail("issue-record path is empty.");
    }
    const std::filesystem::path directory = target.parent_path();
    if (!directory.empty()) {
        const auto ensured = platform::WindowsPaths::ensureDirectory(directory);
        if (!ensured) {
            return fail("could not create the issue-record directory.");
        }
    }
    const std::string text = encodeDocument(records);
    std::error_code inspectError;
    const bool destinationExists = std::filesystem::exists(target, inspectError);
    if (inspectError) {
        return fail("could not inspect the issue-record destination before publication.");
    }
    auto publisher = platform::AtomicFilePublisher::begin(target,
                                                          platform::TemporaryFileIdentity{
                                                              .operation = "issue-record-save",
                                                              .ownerId = "issue-log",
                                                              .revision = 1,
                                                          });
    if (!publisher) {
        return fail("could not begin issue-record publication.");
    }
    const std::span<const char> characters{text.data(), text.size()};
    auto status = publisher.value()->write(std::as_bytes(characters));
    if (!status) {
        return fail("could not write the issue-record document.");
    }
    status = publisher.value()->flush();
    if (!status) {
        return fail("could not flush the issue-record document.");
    }
    status = destinationExists ? publisher.value()->publishReplacingExisting()
                               : publisher.value()->publishNew();
    if (!status) {
        return fail("could not publish the issue-record document.");
    }
    return true;
}

} // namespace dvs::persistence
