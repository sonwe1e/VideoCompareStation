#include "dvs/persistence/SettingsRepository.h"

#include "dvs/platform/AtomicFilePublisher.h"
#include "dvs/platform/WindowsPaths.h"

#include "RepositorySupport.h"
#include "SerialIoActor.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace dvs::persistence {
namespace {

using Json = nlohmann::json;

constexpr std::int64_t kSettingsSchemaVersion = 1;
constexpr std::uintmax_t kMaximumSettingsBytes = std::uintmax_t{1024U} * 1024U;

[[nodiscard]] bool isSettingsSchemaVersion(const Json& value) {
    return (value.is_number_integer() && value.get<std::int64_t>() == kSettingsSchemaVersion) ||
           (value.is_number_unsigned() &&
            value.get<std::uint64_t>() == static_cast<std::uint64_t>(kSettingsSchemaVersion));
}

[[nodiscard]] domain::MediaError settingsError(std::string technicalDetail) {
    return internal::persistenceError(
        domain::MediaErrorCode::kFileIo, std::nullopt, std::move(technicalDetail));
}

[[nodiscard]] domain::Result<std::filesystem::path>
resolveSettingsFile(const std::filesystem::path& configuredPath) {
    if (!configuredPath.empty()) {
        const auto absolutePath = platform::WindowsPaths::absolutePath(configuredPath);
        if (!absolutePath) {
            return domain::Result<std::filesystem::path>::failure(
                internal::platformError(absolutePath.error(), std::nullopt));
        }
        return domain::Result<std::filesystem::path>::success(absolutePath.value());
    }

    const auto applicationPaths = platform::WindowsPaths::applicationDataPaths();
    if (!applicationPaths) {
        return domain::Result<std::filesystem::path>::failure(
            internal::platformError(applicationPaths.error(), std::nullopt));
    }
    return domain::Result<std::filesystem::path>::success(applicationPaths.value().settingsFile);
}

[[nodiscard]] domain::Result<application::SettingsSnapshot>
decodeSettings(const std::string_view text) {
    try {
        const Json document = Json::parse(std::string{text});
        if (!document.is_object()) {
            return domain::Result<application::SettingsSnapshot>::failure(
                settingsError("Settings document must be a JSON object."));
        }

        const auto version = document.find("schemaVersion");
        const auto values = document.find("values");
        if (version == document.end() || !isSettingsSchemaVersion(*version) ||
            values == document.end() || !values->is_object()) {
            return domain::Result<application::SettingsSnapshot>::failure(settingsError(
                "Settings document must contain schemaVersion 1 and an object of values."));
        }

        application::SettingsSnapshot settings;
        for (auto iterator = values->begin(); iterator != values->end(); ++iterator) {
            if (iterator.key().empty() || !iterator.value().is_string()) {
                return domain::Result<application::SettingsSnapshot>::failure(settingsError(
                    "Settings keys must be non-empty and settings values must be strings."));
            }
            settings.values.emplace(iterator.key(), iterator.value().get<std::string>());
        }
        return domain::Result<application::SettingsSnapshot>::success(std::move(settings));
    } catch (const std::exception&) {
        return domain::Result<application::SettingsSnapshot>::failure(
            settingsError("Settings document is not valid UTF-8 JSON."));
    }
}

[[nodiscard]] domain::Result<application::SettingsSnapshot>
readSettings(const std::filesystem::path& configuredPath) {
    auto settingsFile = resolveSettingsFile(configuredPath);
    if (!settingsFile) {
        return domain::Result<application::SettingsSnapshot>::failure(settingsFile.error());
    }

    std::error_code errorCode;
    const bool exists = std::filesystem::exists(settingsFile.value(), errorCode);
    if (errorCode) {
        return domain::Result<application::SettingsSnapshot>::failure(
            settingsError("Could not inspect the settings file."));
    }
    if (!exists) {
        return domain::Result<application::SettingsSnapshot>::success(
            application::SettingsSnapshot{});
    }

    const std::uintmax_t byteCount = std::filesystem::file_size(settingsFile.value(), errorCode);
    if (errorCode || byteCount > kMaximumSettingsBytes ||
        byteCount > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return domain::Result<application::SettingsSnapshot>::failure(
            settingsError("Settings file is unavailable or exceeds the 1 MiB document limit."));
    }

    std::ifstream stream(settingsFile.value(), std::ios::binary);
    if (!stream) {
        return domain::Result<application::SettingsSnapshot>::failure(
            settingsError("Could not open the settings file for reading."));
    }

    std::string text(static_cast<std::size_t>(byteCount), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (stream.gcount() != static_cast<std::streamsize>(text.size())) {
        return domain::Result<application::SettingsSnapshot>::failure(
            settingsError("Settings file changed or could not be fully read."));
    }
    return decodeSettings(text);
}

[[nodiscard]] domain::Status writeSettings(const std::filesystem::path& configuredPath,
                                           const application::SettingsSaveRequest& request,
                                           internal::OperationState& operation) {
    auto settingsFile = resolveSettingsFile(configuredPath);
    if (!settingsFile) {
        return domain::Status::failure(settingsFile.error());
    }

    const std::filesystem::path directory = settingsFile.value().parent_path();
    if (directory.empty()) {
        return domain::Status::failure(
            settingsError("Settings destination has no parent directory."));
    }
    const auto ensureDirectory = platform::WindowsPaths::ensureDirectory(directory);
    if (!ensureDirectory) {
        return domain::Status::failure(
            internal::platformError(ensureDirectory.error(), std::nullopt));
    }

    Json values = Json::object();
    for (const auto& [key, value] : request.settings.values) {
        if (key.empty()) {
            return domain::Status::failure(settingsError("Settings keys must be non-empty."));
        }
        values[key] = value;
    }

    std::string text;
    try {
        text =
            Json{
                {"schemaVersion", kSettingsSchemaVersion},
                {"values", std::move(values)},
            }
                .dump(2);
        text.push_back('\n');
    } catch (const std::exception&) {
        return domain::Status::failure(settingsError("Could not serialize settings JSON."));
    }

    std::error_code errorCode;
    const bool destinationExists = std::filesystem::exists(settingsFile.value(), errorCode);
    if (errorCode) {
        return domain::Status::failure(
            settingsError("Could not inspect the settings destination before publication."));
    }

    auto publisher =
        platform::AtomicFilePublisher::begin(settingsFile.value(),
                                             platform::TemporaryFileIdentity{
                                                 .operation = "settings-save",
                                                 .ownerId = "settings",
                                                 .revision = request.context.requestId.value(),
                                             });
    if (!publisher) {
        return domain::Status::failure(internal::platformError(publisher.error(), std::nullopt));
    }

    const std::span<const char> characters{text.data(), text.size()};
    auto status = publisher.value()->write(std::as_bytes(characters));
    if (!status) {
        return domain::Status::failure(internal::platformError(status.error(), std::nullopt));
    }
    status = publisher.value()->flush();
    if (!status) {
        return domain::Status::failure(internal::platformError(status.error(), std::nullopt));
    }

    if (!operation.tryBeginCommit()) {
        return domain::Status::failure(
            settingsError("Settings save was canceled before atomic publication."));
    }
    status = destinationExists ? publisher.value()->publishReplacingExisting()
                               : publisher.value()->publishNew();
    if (!status) {
        return domain::Status::failure(internal::platformError(status.error(), std::nullopt));
    }
    return domain::Status::success();
}

[[nodiscard]] application::PortSubmitResult
mapSubmitResult(const internal::IoSubmitResult result) noexcept {
    switch (result) {
    case internal::IoSubmitResult::kAccepted:
        return application::PortSubmitResult::Accepted;
    case internal::IoSubmitResult::kBusy:
        return application::PortSubmitResult::Busy;
    case internal::IoSubmitResult::kClosed:
        return application::PortSubmitResult::Closed;
    }
    return application::PortSubmitResult::Closed;
}

} // namespace

class SettingsRepository::Impl final {
public:
    Impl(std::filesystem::path settingsFile, const std::size_t queueCapacity)
        : settingsFile_(std::move(settingsFile)), actor(queueCapacity) {}

    ~Impl() {
        operations.cancelAll();
        actor.close();
    }

    [[nodiscard]] application::PortSubmitResult
    submit(const application::SettingsLoadRequest& request,
           const std::shared_ptr<application::IApplicationEventSink>& events) {
        if (!events) {
            return application::PortSubmitResult::Closed;
        }
        const std::weak_ptr<application::IApplicationEventSink> weakEvents{events};
        return submitTask(request.context, [this, request, weakEvents](const auto& operation) {
            executeLoad(request, weakEvents, operation);
        });
    }

    [[nodiscard]] application::PortSubmitResult
    submit(const application::SettingsSaveRequest& request,
           const std::shared_ptr<application::IApplicationEventSink>& events) {
        if (!events) {
            return application::PortSubmitResult::Closed;
        }
        const std::weak_ptr<application::IApplicationEventSink> weakEvents{events};
        return submitTask(
            request.context,
            [this, request, weakEvents]( // NOLINT(bugprone-exception-escape): actor wraps task()
                const auto& operation) { executeSave(request, weakEvents, operation); });
    }

    void cancel(const application::RequestContext& context) noexcept {
        operations.cancel(context);
    }

private:
    template <typename TTask>
    [[nodiscard]] application::PortSubmitResult
    submitTask(const application::RequestContext& context, TTask&& task) {
        std::shared_ptr<internal::OperationState> operation;
        try {
            operation = operations.add(context);
            const auto submitted = actor.submit(
                // NOLINTBEGIN(bugprone-exception-escape)
                // The IO actor's run loop wraps task() in try/catch; captures cannot escape.
                [operation, task = std::forward<TTask>(task)]() mutable { task(operation); });
            // NOLINTEND(bugprone-exception-escape)
            if (submitted != internal::IoSubmitResult::kAccepted) {
                operations.remove(operation);
            }
            return mapSubmitResult(submitted);
        } catch (...) {
            if (operation) {
                operations.remove(operation);
            }
            return application::PortSubmitResult::Busy;
        }
    }

    void executeLoad(const application::SettingsLoadRequest& request,
                     const std::weak_ptr<application::IApplicationEventSink>& events,
                     const std::shared_ptr<internal::OperationState>& operation) noexcept {
        try {
            if (operation->isCanceled()) {
                internal::completeCanceled(operation, events, request.context);
            } else {
                auto settings = readSettings(settingsFile_);
                if (!settings) {
                    internal::completeFailed(operation, events, request.context, settings.error());
                } else if (operation->isCanceled()) {
                    internal::completeCanceled(operation, events, request.context);
                } else {
                    application::ApplicationEvent loaded{application::SettingsLoaded{
                        .context = request.context,
                        .settings = std::move(settings).value(),
                    }};
                    if (operation->isCanceled()) {
                        internal::completeCanceled(operation, events, request.context);
                    } else if (operation->claimTerminal()) {
                        internal::postCritical(events, std::move(loaded));
                        internal::postSucceeded(events, request.context);
                    }
                }
            }
        } catch (const std::exception& exception) {
            internal::completeFailed(operation,
                                     events,
                                     request.context,
                                     settingsError("Unexpected settings-load exception: " +
                                                   std::string{exception.what()}));
        } catch (...) {
            internal::completeFailed(operation,
                                     events,
                                     request.context,
                                     settingsError("Unexpected settings-load failure."));
        }
        operations.remove(operation);
    }

    void executeSave(const application::SettingsSaveRequest& request,
                     const std::weak_ptr<application::IApplicationEventSink>& events,
                     const std::shared_ptr<internal::OperationState>& operation) noexcept {
        try {
            if (operation->isCanceled()) {
                internal::completeCanceled(operation, events, request.context);
            } else {
                const auto status = writeSettings(settingsFile_, request, *operation);
                if (!status) {
                    internal::completeFailed(operation, events, request.context, status.error());
                } else if (operation->claimTerminal()) {
                    internal::postSucceeded(events, request.context);
                }
            }
        } catch (const std::exception& exception) {
            internal::completeFailed(operation,
                                     events,
                                     request.context,
                                     settingsError("Unexpected settings-save exception: " +
                                                   std::string{exception.what()}));
        } catch (...) {
            internal::completeFailed(operation,
                                     events,
                                     request.context,
                                     settingsError("Unexpected settings-save failure."));
        }
        operations.remove(operation);
    }

    std::filesystem::path settingsFile_;
    internal::SerialIoActor actor;
    internal::OperationRegistry operations;
};

SettingsRepository::SettingsRepository(std::filesystem::path settingsFile,
                                       const std::size_t queueCapacity)
    : impl_(std::make_unique<Impl>(std::move(settingsFile), queueCapacity)) {}

SettingsRepository::~SettingsRepository() = default;

application::PortSubmitResult SettingsRepository::submit(
    const application::SettingsLoadRequest& request,
    std::shared_ptr<application::IApplicationEventSink>
        events) { // NOLINT(performance-unnecessary-value-param): port signature
    return impl_->submit(request, events);
}

application::PortSubmitResult SettingsRepository::submit(
    const application::SettingsSaveRequest& request,
    std::shared_ptr<application::IApplicationEventSink>
        events) { // NOLINT(performance-unnecessary-value-param): port signature
    return impl_->submit(request, events);
}

void SettingsRepository::cancel(const application::RequestContext& context) noexcept {
    impl_->cancel(context);
}

} // namespace dvs::persistence
