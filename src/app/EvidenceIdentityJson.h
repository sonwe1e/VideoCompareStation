#pragma once

#include <QJsonValue>
#include <QString>
#include <QVariant>

namespace dvs::app::evidence {

// Forensic JSON representation for 64-bit identity fields (request id, frame id, session
// epoch, playback generation). The historical truncation to 32 bits came from narrowing
// through uint (0x100000001 became 1) - not from the JSON layer. The decimal string is used
// anyway for two narrower reasons: it removes the explicit uint narrowing as a failure mode,
// and it guarantees precision for any consumer that parses the report as JSON numbers (JS
// doubles lose integer exactness above 2^53, and QJsonValue::toDouble has the same ceiling)
// or passes it through JS tooling. A missing observation stays the literal "unknown" instead
// of a defaulted 0 masquerading as a reading. Code values and coordinates are real uint
// values and keep using plain JSON numbers (see codeValueOrUnknown below).
[[nodiscard]] inline QJsonValue identity64OrUnknown(const QVariant& value) {
    if (!value.isValid()) {
        return QJsonValue{QStringLiteral("unknown")};
    }
    bool converted = false;
    const qulonglong identity = value.toULongLong(&converted);
    if (!converted) {
        return QJsonValue{QStringLiteral("unknown")};
    }
    return QJsonValue{QString::number(identity)};
}

[[nodiscard]] inline QJsonValue identity64OrUnknown(const qulonglong identity) {
    return QJsonValue{QString::number(identity)};
}

// Forensic JSON representation for code values and pixel coordinates: plain JSON numbers. A
// missing observation becomes the literal "unknown" rather than a defaulted 0.
[[nodiscard]] inline QJsonValue codeValueOrUnknown(const QVariant& value) {
    if (!value.isValid()) {
        return QJsonValue{QStringLiteral("unknown")};
    }
    bool converted = false;
    const uint code = value.toUInt(&converted);
    if (!converted) {
        return QJsonValue{QStringLiteral("unknown")};
    }
    return QJsonValue{static_cast<double>(code)};
}

} // namespace dvs::app::evidence
