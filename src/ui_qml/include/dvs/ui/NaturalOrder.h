#pragma once

#include <QChar>
#include <QString>

namespace dvs::ui {

// Compare arbitrary-length decimal runs without integer conversion, locale/ICU dependencies
// or overflow. Equal numeric spellings use the original name as a deterministic tie-breaker.
// Shared by the video folder list and the still-image folder cursor so both walk a directory
// in the same human order.
//
// Non-ASCII names fall back to code-unit order. That is deliberate: a locale collator would make
// the order depend on the machine's ICU data and locale, so two people looking at the same
// folder could disagree about "the next image". Determinism beats pinyin here (D-013).
[[nodiscard]] inline bool naturalLess(const QString& first, const QString& second) {
    const QString a = first.toCaseFolded();
    const QString b = second.toCaseFolded();
    const auto digit = [](const QChar c) { return c >= u'0' && c <= u'9'; };
    qsizetype i = 0;
    qsizetype j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            qsizetype endA = i;
            qsizetype endB = j;
            while (endA < a.size() && digit(a[endA])) {
                ++endA;
            }
            while (endB < b.size() && digit(b[endB])) {
                ++endB;
            }
            while (i < endA && a[i] == u'0') {
                ++i;
            }
            while (j < endB && b[j] == u'0') {
                ++j;
            }
            if (endA - i != endB - j) {
                return endA - i < endB - j;
            }
            while (i < endA) {
                if (a[i] != b[j]) {
                    return a[i] < b[j];
                }
                ++i;
                ++j;
            }
            i = endA;
            j = endB;
        } else {
            if (a[i] != b[j]) {
                return a[i] < b[j];
            }
            ++i;
            ++j;
        }
    }
    if (i < a.size() || j < b.size()) {
        return i == a.size();
    }
    return first < second;
}

} // namespace dvs::ui
