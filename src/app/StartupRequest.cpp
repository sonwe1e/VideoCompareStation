#include "StartupRequest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <utility>

namespace dvs::app {
namespace {

constexpr qsizetype kMaximumRequestBytes = qsizetype{64} * 1024;

[[nodiscard]] std::filesystem::path pathFromQString(const QString& value) {
    return std::filesystem::path{value.toStdWString()};
}

[[nodiscard]] QString kindName(const StartupRequest::Kind kind) {
    switch (kind) {
    case StartupRequest::Kind::Empty:
        return QStringLiteral("empty");
    case StartupRequest::Kind::PlaySingle:
        return QStringLiteral("play-single");
    case StartupRequest::Kind::Compare:
        return QStringLiteral("compare");
    }
    return {};
}

[[nodiscard]] std::optional<StartupRequest::Kind> kindFromName(const QString& name) {
    if (name == QStringLiteral("empty")) {
        return StartupRequest::Kind::Empty;
    }
    if (name == QStringLiteral("play-single")) {
        return StartupRequest::Kind::PlaySingle;
    }
    if (name == QStringLiteral("compare")) {
        return StartupRequest::Kind::Compare;
    }
    return std::nullopt;
}

[[nodiscard]] StartupRequestParseResult validate(StartupRequest request) {
    const std::size_t count = request.sources.size();
    const bool countValid =
        (request.kind == StartupRequest::Kind::Empty && count == 0U) ||
        (request.kind == StartupRequest::Kind::PlaySingle && count == 1U) ||
        (request.kind == StartupRequest::Kind::Compare && (count == 2U || count == 3U));
    if (!countValid) {
        return {.error = QStringLiteral("The startup request has an invalid source count.")};
    }
    if (std::ranges::any_of(request.sources, [](const auto& source) { return source.empty(); })) {
        return {.error = QStringLiteral("Startup source paths must not be empty.")};
    }
    return {.request = std::move(request)};
}

} // namespace

StartupRequestParseResult parseStartupRequest(const QStringList& arguments) {
    if (arguments.isEmpty()) {
        return {.error = QStringLiteral("The process argument list is empty.")};
    }
    const QStringList values = arguments.sliced(1);
    if (values.isEmpty()) {
        return {.request = StartupRequest{}};
    }
    if (values.size() == 2 && values.front() == QStringLiteral("--play")) {
        return validate(StartupRequest{
            .kind = StartupRequest::Kind::PlaySingle,
            .sources = {pathFromQString(values.back())},
        });
    }
    if ((values.size() == 3 || values.size() == 4) &&
        values.front() == QStringLiteral("--compare")) {
        std::vector<std::filesystem::path> sources;
        sources.reserve(static_cast<std::size_t>(values.size() - 1));
        for (qsizetype index = 1; index < values.size(); ++index) {
            sources.push_back(pathFromQString(values[index]));
        }
        return validate(StartupRequest{
            .kind = StartupRequest::Kind::Compare,
            .sources = std::move(sources),
        });
    }
    if (values.size() >= 1 && values.size() <= 3 &&
        std::ranges::none_of(
            values, [](const QString& value) { return value.startsWith(QStringLiteral("--")); })) {
        std::vector<std::filesystem::path> sources;
        sources.reserve(static_cast<std::size_t>(values.size()));
        for (const QString& value : values) {
            sources.push_back(pathFromQString(value));
        }
        return validate(StartupRequest{
            .kind = values.size() == 1 ? StartupRequest::Kind::PlaySingle
                                       : StartupRequest::Kind::Compare,
            .sources = std::move(sources),
        });
    }
    return {
        .error = QStringLiteral("Unsupported GUI arguments. Pass one to three video paths, or use "
                                "--play <video> / --compare <video...>."),
    };
}

QByteArray encodeStartupRequest(const StartupRequest& request) {
    const StartupRequestParseResult checked = validate(request);
    if (!checked) {
        return {};
    }
    QJsonArray sources;
    for (const auto& source : request.sources) {
        sources.push_back(QString::fromStdWString(source.wstring()));
    }
    const QJsonObject object{
        {QStringLiteral("version"), 1},
        {QStringLiteral("kind"), kindName(request.kind)},
        {QStringLiteral("sources"), sources},
    };
    const QByteArray payload = QJsonDocument{object}.toJson(QJsonDocument::Compact);
    return payload.size() <= kMaximumRequestBytes ? payload : QByteArray{};
}

StartupRequestParseResult decodeStartupRequest(const QByteArray& payload) {
    if (payload.isEmpty() || payload.size() > kMaximumRequestBytes) {
        return {.error = QStringLiteral("The startup request payload size is invalid.")};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {.error = QStringLiteral("The startup request is not valid JSON.")};
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt(-1) != 1 ||
        !object.value(QStringLiteral("kind")).isString() ||
        !object.value(QStringLiteral("sources")).isArray()) {
        return {.error = QStringLiteral("The startup request schema is invalid.")};
    }
    const auto kind = kindFromName(object.value(QStringLiteral("kind")).toString());
    if (!kind) {
        return {.error = QStringLiteral("The startup request kind is unknown.")};
    }
    std::vector<std::filesystem::path> sources;
    const QJsonArray encodedSources = object.value(QStringLiteral("sources")).toArray();
    sources.reserve(static_cast<std::size_t>(encodedSources.size()));
    for (const auto& source : encodedSources) {
        if (!source.isString()) {
            return {.error = QStringLiteral("Startup source paths must be strings.")};
        }
        sources.push_back(pathFromQString(source.toString()));
    }
    return validate(StartupRequest{.kind = *kind, .sources = std::move(sources)});
}

} // namespace dvs::app
