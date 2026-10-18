#include "dvs/persistence/IssueRecordRepository.h"

#include "dvs/platform/AtomicFilePublisher.h"
#include "dvs/platform/WindowsPaths.h"

#include <cmath>
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

constexpr std::uintmax_t kMaximumIssueDocumentBytes = std::uintmax_t{2U} * 1024U * 1024U;

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

// Typed readers for the defensive parse ladder: each assigns only when the key exists and
// carries the expected JSON type, so a missing or mistyped field keeps the record default -
// the behavior every hand-rolled contains()+is_X() chain below used to spell out. The bool
// return is informational for the few sites that branch on the field being present.
bool readString(const Json& value, const char* const key, std::string* out) {
    const auto field = value.find(key);
    if (field == value.end() || !field->is_string()) {
        return false;
    }
    *out = field->get<std::string>();
    return true;
}

template <typename Integer>
bool readInteger(const Json& value, const char* const key, Integer* out) {
    const auto field = value.find(key);
    if (field == value.end() || !field->is_number_integer()) {
        return false;
    }
    *out = field->get<Integer>();
    return true;
}

bool readBoolean(const Json& value, const char* const key, bool* out) {
    const auto field = value.find(key);
    if (field == value.end() || !field->is_boolean()) {
        return false;
    }
    *out = field->get<bool>();
    return true;
}

// Session identity fields are unsigned; accept either JSON number flavor but never a negative,
// mirroring the alignmentRevision tolerance for hand-edited documents.
void readUnsigned64(const Json& value, const char* const key, std::uint64_t* out) {
    const auto field = value.find(key);
    if (field == value.end()) {
        return;
    }
    if (field->is_number_unsigned()) {
        *out = field->get<std::uint64_t>();
    } else if (field->is_number_integer()) {
        const auto signedValue = field->get<std::int64_t>();
        *out = signedValue > 0 ? static_cast<std::uint64_t>(signedValue) : 0U;
    }
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
    readString(value, "path", &source.path);
    readInteger(value, "byteSize", &source.byteSize);
    readInteger(value, "modifiedUtcMilliseconds", &source.modifiedUtcMilliseconds);
    readString(value, "fingerprintSha256", &source.fingerprintSha256);
    readBoolean(value, "hasPresentation", &source.hasPresentation);
    readInteger(value, "displayIndex", &source.displayIndex);
    readInteger(value, "presentationTimestampTicks", &source.presentationTimestampTicks);
    readInteger(value, "timeBaseNumerator", &source.timeBaseNumerator);
    readInteger(value, "timeBaseDenominator", &source.timeBaseDenominator);
    readInteger(value, "presentationMatchKind", &source.presentationMatchKind);
    readInteger(value, "presentationMissingReason", &source.presentationMissingReason);
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
    value["referenceSourceIndex"] = record.referenceSourceIndex;
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
    // E1: valid wipe pairs are written as the exact {orientation, ratio}; the invalid
    // sentinel (-2) is written explicitly instead of being omitted, so a re-save can never
    // masquerade rejected data as a legacy record that simply lacked the fields. Absent on
    // write only when the record itself carries no wipe state (-1), which keeps legacy files
    // byte-identical to what older writers produced.
    if (record.view.wipeOrientation == -2) {
        value["view"]["wipeOrientation"] = -2;
        value["view"]["wipePosition"] = -2.0;
    } else if (record.view.wipeOrientation >= 0 && record.view.wipePosition >= 0.0) {
        value["view"]["wipeOrientation"] = record.view.wipeOrientation;
        value["view"]["wipePosition"] = record.view.wipePosition;
    }
    // FLOW1 image observation settings are written only when captured (-1 = legacy/absent),
    // so records captured outside the image workspace stay byte-identical to older files.
    if (record.view.imageViewMode >= 0) {
        value["view"]["imageViewMode"] = record.view.imageViewMode;
    }
    if (record.view.imageBackgroundMode >= 0) {
        value["view"]["imageBackgroundMode"] = record.view.imageBackgroundMode;
    }
    if (record.view.imageDiffGain >= 0) {
        value["view"]["imageDiffGain"] = record.view.imageDiffGain;
    }
    if (record.view.imageAlphaFilterEnabled >= 0) {
        value["view"]["imageAlphaFilterEnabled"] = record.view.imageAlphaFilterEnabled;
    }
    if (record.view.imageAlphaThreshold >= 0) {
        value["view"]["imageAlphaThreshold"] = record.view.imageAlphaThreshold;
    }
    // Written only when the record carries a diagnostic interval, so legacy files stay
    // byte-identical to what older writers produced.
    if (record.interval.anchorFrame >= 0) {
        value["interval"] = Json{
            {"anchorFrame", record.interval.anchorFrame},
            {"inFrame", record.interval.inFrame},
            {"outFrame", record.interval.outFrame},
            {"sessionEpoch", record.interval.sessionEpoch},
            {"playbackGeneration", record.interval.playbackGeneration},
            {"loopSeamAtOut", record.interval.loopSeamAtOut},
        };
    }
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
    readInteger(value, "schemaVersion", &record.schemaVersion);
    std::string kind;
    if (readString(value, "kind", &kind)) {
        record.kind = kind == "image-pair" ? IssueRecordKind::ImagePair : IssueRecordKind::Video;
    }
    readInteger(value, "createdAtUtcMilliseconds", &record.createdAtUtcMilliseconds);
    readString(value, "note", &record.note);
    if (value.contains("screenshot") && value["screenshot"].is_object()) {
        const Json& screenshot = value["screenshot"];
        record.screenshotIsDisplayResult = true;
        readString(screenshot, "path", &record.screenshotPath);
        std::string screenshotKind;
        if (readString(screenshot, "kind", &screenshotKind) && screenshotKind != "display-result") {
            record.screenshotIsDisplayResult = false;
        }
    }
    readBoolean(value, "hasValidPresentation", &record.hasValidPresentation);
    readInteger(value, "canonicalSourceIndex", &record.canonicalSourceIndex);
    // Absent in records written before the field existed; the -1 default then makes restore
    // fall back to the canonical index instead of inventing a reference slot.
    readInteger(value, "referenceSourceIndex", &record.referenceSourceIndex);
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
        readBoolean(view, "roiEnabled", &record.view.roiEnabled);
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
        readBoolean(view, "resampleAllowed", &record.view.resampleAllowed);
        // FLOW1: image observation settings are optional and validated individually. A
        // missing, mistyped or out-of-range field becomes -1 ("not captured") so restore
        // reports a partial observation instead of clamping bad data into a plausible state.
        auto readBounded = [&view](const char* key, const int minimum, const int maximum) {
            if (!view.contains(key) || !view[key].is_number_integer()) {
                return std::int32_t{-1};
            }
            if (view[key] < minimum || view[key] > maximum) {
                return std::int32_t{-1};
            }
            return view[key].get<std::int32_t>();
        };
        record.view.imageViewMode = readBounded("imageViewMode", 0, 2);
        record.view.imageBackgroundMode = readBounded("imageBackgroundMode", 0, 3);
        record.view.imageDiffGain = readBounded("imageDiffGain", 1, 16);
        record.view.imageAlphaFilterEnabled = readBounded("imageAlphaFilterEnabled", 0, 1);
        record.view.imageAlphaThreshold = readBounded("imageAlphaThreshold", 0, 255);
        // E1: the wipe pair is validated as a pair. Absent in legacy records (-1/-1 = "no
        // wipe state"); present-but-invalid (wrong type, half-present, orientation outside
        // {0,1} - including wide integers that would narrow into 0/1 - ratio outside [0,1]
        // or non-finite) is rejected to an explicit invalid sentinel (-2) instead of being
        // clamped into a plausible-looking observation.
        const bool hasOrientation = view.contains("wipeOrientation");
        const bool hasPosition = view.contains("wipePosition");
        if (hasOrientation != hasPosition) {
            record.view.wipeOrientation = -2;
            record.view.wipePosition = -2.0;
        } else if (hasOrientation && hasPosition) {
            const Json& orientation = view["wipeOrientation"];
            const Json& position = view["wipePosition"];
            // Compare against the raw JSON values first: a wide integer such as 4294967296
            // would narrow to 0 through get<int>() and sneak past a {0,1} membership check.
            const bool typesValid = orientation.is_number_integer() &&
                                    (orientation == 0 || orientation == 1) && position.is_number();
            bool pairValid = false;
            if (typesValid) {
                const double positionValue = position.get<double>();
                pairValid =
                    std::isfinite(positionValue) && positionValue >= 0.0 && positionValue <= 1.0;
            }
            if (pairValid) {
                record.view.wipeOrientation = orientation.get<int>();
                record.view.wipePosition = position.get<double>();
            } else {
                record.view.wipeOrientation = -2;
                record.view.wipePosition = -2.0;
            }
        }
    }
    // Absent in records written before the interval existed; the defaults then keep the record
    // at "no diagnostic interval" instead of inventing a neighborhood.
    if (value.contains("interval") && value["interval"].is_object()) {
        const Json& interval = value["interval"];
        readInteger(interval, "anchorFrame", &record.interval.anchorFrame);
        readInteger(interval, "inFrame", &record.interval.inFrame);
        readInteger(interval, "outFrame", &record.interval.outFrame);
        readUnsigned64(interval, "sessionEpoch", &record.interval.sessionEpoch);
        readUnsigned64(interval, "playbackGeneration", &record.interval.playbackGeneration);
        readBoolean(interval, "loopSeamAtOut", &record.interval.loopSeamAtOut);
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
        readInteger(pair, "rowIndex", &record.rowIndex);
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
