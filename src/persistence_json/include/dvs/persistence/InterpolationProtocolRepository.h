#pragma once

#include "dvs/application/InterpolationProtocolImport.h"

#include <filesystem>
#include <string>

namespace dvs::persistence {

// P03 JSON import for interpolation protocols (frame-provenance manifests). Schema version 1
// only; an unknown version or kind is rejected explicitly instead of guessed, and a failed load
// never returns a partial protocol. The JSON carries zero-based output/GT indices; this adapter
// is the only place that index base is stated for the file format.
//
// Document forms (schema_version == 1):
//   decimation: {"schema_version":1,"kind":"decimation","gt_frame_count":N,"stride":S,
//                "phase":P,"tail_policy":"trim"|"mark_missing",
//                "mapping_origin":"imported_manifest"|"manual"}
//   explicit:   {"schema_version":1,"kind":"explicit","gt_frame_count":N,
//                "mapping_origin":"imported_manifest"|"manual",
//                "origins":[{"type":"retained","gt":i} |
//                           {"type":"generated","input0":a,"input1":b,"ratio":[num,den]} |
//                           {"type":"unknown"} |
//                           {"type":"missing","expected":e|null}, ...]}
// mapping_origin is optional in both forms (default "imported_manifest"); "suggestion" is never
// importable because a suggestion is unconfirmed in-app state, and encodeDocument() likewise
// only round-trips committed origins.
class InterpolationProtocolRepository final : public application::IInterpolationProtocolRepository {
public:
    InterpolationProtocolRepository() = default;
    ~InterpolationProtocolRepository() override = default;

    InterpolationProtocolRepository(const InterpolationProtocolRepository&) = delete;
    InterpolationProtocolRepository& operator=(const InterpolationProtocolRepository&) = delete;
    InterpolationProtocolRepository(InterpolationProtocolRepository&&) = delete;
    InterpolationProtocolRepository& operator=(InterpolationProtocolRepository&&) = delete;

    // Whole-file read plus decode; const and stateless, so the UI controller may call it from a
    // worker thread.
    [[nodiscard]] application::InterpolationProtocolIoResult
    load(const std::filesystem::path& path) const override;

    // Pure decode with no filesystem access. Distinct errors for invalid JSON, unsupported
    // schema_version, unknown kind, malformed fields, and domain validation failure.
    [[nodiscard]] static application::InterpolationProtocolIoResult
    decodeDocument(const std::string& text);

    // Canonical serialization of the explicit form; decodeDocument(encodeDocument(p)) == p for
    // every protocol whose mapping origin is imported_manifest or manual.
    [[nodiscard]] static std::string encodeDocument(const domain::InterpolationProtocol& protocol);
};

} // namespace dvs::persistence
