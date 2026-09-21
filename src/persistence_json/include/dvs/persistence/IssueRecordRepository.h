#pragma once

#include "dvs/application/IssueRecord.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dvs::persistence {

struct IssueRecordDocument final {
    bool ok = false;
    // Set when ok == false.
    std::string error;
    std::vector<application::IssueRecord> records;
};

// Manual save/load for T7 issue records. Versioned JSON only; unknown schemaVersion is
// rejected with an explanation and the on-disk file is never rewritten by a failed load.
class IssueRecordRepository final : public application::IIssueRecordRepository {
public:
    explicit IssueRecordRepository(std::filesystem::path documentPath = {});
    ~IssueRecordRepository() override;

    [[nodiscard]] const std::filesystem::path& documentPath() const noexcept;

    [[nodiscard]] application::IssueRecordIoResult
    load(const std::filesystem::path& path) const override;
    [[nodiscard]] bool save(const std::filesystem::path& path,
                            const std::vector<application::IssueRecord>& records,
                            std::string* error) const override;

    // Legacy helpers used by tests; load/save are the port surface.
    [[nodiscard]] IssueRecordDocument loadDocument() const;
    [[nodiscard]] IssueRecordDocument loadDocumentFrom(const std::filesystem::path& path) const;
    [[nodiscard]] bool saveDocument(const std::vector<application::IssueRecord>& records,
                                    std::string* error) const;

    [[nodiscard]] static std::string
    encodeDocument(const std::vector<application::IssueRecord>& records);
    [[nodiscard]] static IssueRecordDocument decodeDocument(const std::string& text);

private:
    std::filesystem::path documentPath_;
};

} // namespace dvs::persistence
