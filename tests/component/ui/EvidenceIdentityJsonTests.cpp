#include "EvidenceIdentityJson.h"

#include <QJsonObject>

#include <gtest/gtest.h>

namespace dvs::app::evidence {
namespace {

// Forensic identity fields keep the full 64-bit width: the JSON value is the exact decimal
// string, round-trippable to the same qulonglong. The 0x100000001 probe is the regression
// for the uint narrowing that recorded it as 1; the string form additionally removes the
// >2^53 exact-integer cliff that JSON doubles would otherwise impose on consumers.
TEST(EvidenceIdentityJsonTests, Identity64StaysExactBeyondTheDoubleRange) {
    constexpr qulonglong wide = 0x1'0000'0001ULL;
    const QJsonValue value = identity64OrUnknown(wide);
    ASSERT_TRUE(value.isString());
    const QString text = value.toString();
    EXPECT_EQ(text.toULongLong(), wide);
    EXPECT_EQ(text, QStringLiteral("4294967297"));

    constexpr qulonglong maxIdentity = 0xFFFF'FFFF'FFFF'FFFFULL;
    const QJsonValue maxValue = identity64OrUnknown(maxIdentity);
    ASSERT_TRUE(maxValue.isString());
    EXPECT_EQ(maxValue.toString().toULongLong(), maxIdentity);

    const QJsonValue fromVariant = identity64OrUnknown(QVariant{wide});
    ASSERT_TRUE(fromVariant.isString());
    EXPECT_EQ(fromVariant.toString().toULongLong(), wide);
}

TEST(EvidenceIdentityJsonTests, MissingObservationsStayUnknownAndCodesStayNumbers) {
    const QJsonValue missing = identity64OrUnknown(QVariant{});
    ASSERT_TRUE(missing.isString());
    EXPECT_EQ(missing.toString(), QStringLiteral("unknown"));

    const QJsonValue missingCode = codeValueOrUnknown(QVariant{});
    ASSERT_TRUE(missingCode.isString());
    EXPECT_EQ(missingCode.toString(), QStringLiteral("unknown"));

    // Code values and coordinates are real uint values: plain JSON numbers.
    const QJsonValue code = codeValueOrUnknown(QVariant{255U});
    ASSERT_TRUE(code.isDouble());
    EXPECT_EQ(code.toInt(), 255);

    // A small identity in the string form still round-trips; consumers parse it as a number.
    const QJsonValue small = identity64OrUnknown(QVariant{7U});
    ASSERT_TRUE(small.isString());
    EXPECT_EQ(small.toString().toULongLong(), 7ULL);
}

} // namespace
} // namespace dvs::app::evidence
