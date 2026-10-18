# Regression cases for cmake/NormalizeQmlTypes.cmake.
#
# The published Dvs.Ui .qmltypes feeds qmllint and qmlls; a corrupted or drifted artifact
# must be rejected by the generation step itself, with counted checks, instead of flowing
# into a green build. Each case runs the real script in a scratch directory and asserts the
# expected verdict (success or failure). The driver also proves a failing run leaves an
# already-published artifact byte-for-byte unchanged, and asserts its own executed counts.
#
# Inputs: DVS_QMLTYPES_SCRIPT (the script under test), DVS_TEST_ROOT (scratch directory).

if(NOT DEFINED DVS_QMLTYPES_SCRIPT OR NOT DEFINED DVS_TEST_ROOT)
    message(FATAL_ERROR "CheckQmlTypesContractTests requires -DDVS_QMLTYPES_SCRIPT=<file> and -DDVS_TEST_ROOT=<dir>.")
endif()
if(NOT EXISTS "${DVS_QMLTYPES_SCRIPT}")
    message(FATAL_ERROR "Script under test does not exist: ${DVS_QMLTYPES_SCRIPT}")
endif()

set(work "${DVS_TEST_ROOT}")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")

set(case_count 0)
set(content_check_count 0)
set(expected_case_count 11)
set(expected_content_check_count 7)

# A minimal but structurally complete module description: all five components, the two
# controllers uncreatable, the pilot-critical members, and one C++-spelled property type
# that normalization must rewrite.
set(baseline [=[
import QtQuick.tooling 1.2

Module {
    Component {
        file: "DvsQmlTypes.h"
        lineNumber: 54
        name: "dvs::ui::ColorClipboardController"
        accessSemantics: "reference"
        prototype: "QObject"
        exports: ["Dvs.Ui/ColorClipboardController 1.0"]
        isCreatable: false
        exportMetaObjectRevisions: [256]
        Property {
            name: "statusText"
            type: "QString"
            read: "statusText"
            notify: "statusChanged"
        }
        Signal { name: "statusChanged" }
        Method {
            name: "copyText"
            type: "bool"
            Parameter { name: "text"; type: "QString" }
        }
    }
    Component {
        file: "DvsQmlTypes.h"
        lineNumber: 29
        name: "dvs::ui::ComparisonSurface"
        accessSemantics: "reference"
        prototype: "QQuickItem"
        exports: ["Dvs.Ui/ComparisonSurface 1.0"]
        exportMetaObjectRevisions: [256]
        Enum {
            name: "ViewMode"
            values: ["SideBySide", "Wipe"]
        }
        Property {
            name: "wipePosition"
            type: "qreal"
            read: "wipePosition"
        }
    }
    Component {
        file: "DvsQmlTypes.h"
        lineNumber: 41
        name: "dvs::ui::HoldKeyHandler"
        accessSemantics: "reference"
        prototype: "QObject"
        exports: ["Dvs.Ui/HoldKeyHandler 1.0"]
        exportMetaObjectRevisions: [256]
    }
    Component {
        file: "DvsQmlTypes.h"
        lineNumber: 17
        name: "dvs::ui::ImageReviewController"
        accessSemantics: "reference"
        prototype: "QObject"
        exports: ["Dvs.Ui/ImageReviewController 1.0"]
        isCreatable: false
        exportMetaObjectRevisions: [256]
        Enum {
            name: "NumericFormat"
            values: ["IntegerCodeValues", "NormalizedFloat"]
        }
        Enum {
            name: "ProbeColorFormat"
            values: ["ProbeRgb", "ProbeRgba", "ProbeHex"]
        }
        Property {
            name: "numericFormat"
            type: "int"
        }
        Property {
            name: "frozenProbeSample"
            type: "QVariantMap"
        }
        Property {
            name: "changedPixels"
            type: "qint64"
        }
        Method { name: "channelText"; type: "QString"; Parameter { name: "channel8"; type: "int" } }
        Method { name: "channelText16"; type: "QString"; Parameter { name: "channel16"; type: "int" } }
        Method { name: "probeColorText"; type: "QString"; Parameter { name: "format"; type: "int" } }
        Method { name: "captureProbeSample"; type: "bool" }
    }
    Component {
        file: "DvsQmlTypes.h"
        lineNumber: 35
        name: "dvs::ui::JogKeyHandler"
        accessSemantics: "reference"
        prototype: "QObject"
        exports: ["Dvs.Ui/JogKeyHandler 1.0"]
        exportMetaObjectRevisions: [256]
    }
}
]=])

macro(dvs_contract_case name content expect_success)
    math(EXPR case_count "${case_count} + 1")
    set(case_input "${work}/${name}-input.qmltypes")
    set(case_output "${work}/${name}-output.qmltypes")
    file(WRITE "${case_input}" "${content}")
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}"
            "-DINPUT=${case_input}"
            "-DOUTPUT=${case_output}"
            -P "${DVS_QMLTYPES_SCRIPT}"
        RESULT_VARIABLE case_result
        OUTPUT_VARIABLE case_stdout
        ERROR_VARIABLE case_stderr
    )
    if(${expect_success})
        if(NOT case_result EQUAL 0)
            message(FATAL_ERROR "Case '${name}' must succeed but failed: ${case_stderr}")
        endif()
    else()
        if(case_result EQUAL 0)
            message(FATAL_ERROR "Case '${name}' must fail but succeeded: ${case_stdout}")
        endif()
    endif()
    message(STATUS "qmltypes contract case '${name}': ${case_result}")
endmacro()

macro(dvs_content_check description)
    math(EXPR content_check_count "${content_check_count} + 1")
    if(NOT (${ARGN}))
        message(FATAL_ERROR "Content check failed: ${description}")
    endif()
endmacro()

# 1. The accept path: the valid baseline publishes an artifact with normalized spellings.
dvs_contract_case(valid "${baseline}" TRUE)
if(NOT EXISTS "${work}/valid-output.qmltypes")
    message(FATAL_ERROR "The valid case must publish its output artifact.")
endif()
file(READ "${work}/valid-output.qmltypes" valid_output)
string(FIND "${valid_output}" "qreal" valid_qreal)
dvs_content_check("no qreal spelling may survive" valid_qreal EQUAL -1)
string(FIND "${valid_output}" "\"double\"" valid_double)
dvs_content_check("the double spelling must appear" valid_double GREATER -1)
string(FIND "${valid_output}" "\"qlonglong\"" valid_qlonglong)
dvs_content_check("the qlonglong spelling must appear" valid_qlonglong GREATER -1)

# 2. A missing component is rejected.
string(REPLACE "name: \"dvs::ui::JogKeyHandler\"" "name: \"dvs::ui::JogKeyHandlerRemoved\"" missing_component "${baseline}")
dvs_contract_case(missing-component "${missing_component}" FALSE)

# 3. A duplicated component is rejected.
string(REPLACE "name: \"dvs::ui::JogKeyHandler\"" "name: \"dvs::ui::JogKeyHandler\"\n        name: \"dvs::ui::JogKeyHandler\"" duplicated_component "${baseline}")
dvs_contract_case(duplicate-component "${duplicated_component}" FALSE)

# 4. A missing key member is rejected.
string(REPLACE "Method { name: \"channelText\"; type: \"QString\"; Parameter { name: \"channel8\"; type: \"int\" } }\n" "" missing_member "${baseline}")
dvs_contract_case(missing-member "${missing_member}" FALSE)

# 5. Flipping a controller to creatable is rejected.
string(REPLACE "exports: [\"Dvs.Ui/ImageReviewController 1.0\"]\n        isCreatable: false" "exports: [\"Dvs.Ui/ImageReviewController 1.0\"]" creatable_flipped "${baseline}")
dvs_contract_case(creatable-flipped "${creatable_flipped}" FALSE)

# 6. An unresolvable C++ spelling that normalization does not rewrite is rejected.
string(APPEND baseline_with_leftover "${baseline}")
string(APPEND baseline_with_leftover "        Property { name: \"leftover\"; type: \"qint32\" }\n")
dvs_contract_case(leftover-spelling "${baseline_with_leftover}" FALSE)

# 7. An empty input is rejected.
dvs_contract_case(empty "" FALSE)

# 8. A truncated input is rejected.
string(LENGTH "${baseline}" baseline_length)
math(EXPR baseline_half "${baseline_length} / 2")
string(SUBSTRING "${baseline}" 0 ${baseline_half} truncated)
dvs_contract_case(truncated "${truncated}" FALSE)

# 9. An input that already uses the QML spellings passes without changes.
string(REPLACE "\"qreal\"" "\"double\"" already_normalized "${baseline}")
string(REPLACE "\"qint64\"" "\"qlonglong\"" already_normalized "${already_normalized}")
dvs_contract_case(already-normalized "${already_normalized}" TRUE)

# 10. A rename that touches only the QML export (the C++ class name stays) is rejected: the
# contract reads the export identity, not the class name, so this drift cannot pass.
string(REPLACE "exports: [\"Dvs.Ui/ColorClipboardController 1.0\"]" "exports: [\"Dvs.Ui/ClipboardRenamed 1.0\"]" export_renamed "${baseline}")
dvs_contract_case(export-renamed "${export_renamed}" FALSE)

# 11. A tail that lost only its final closing brace (every type and member intact) is
# rejected: the module no longer balances and closes, so the half-file cannot be published.
string(REGEX REPLACE "[ \t\r\n]+$" "" baseline_trimmed "${baseline}")
string(LENGTH "${baseline_trimmed}" baseline_trimmed_length)
math(EXPR baseline_tail_cut "${baseline_trimmed_length} - 1")
string(SUBSTRING "${baseline_trimmed}" 0 ${baseline_tail_cut} tail_truncated)
dvs_contract_case(tail-truncated "${tail_truncated}" FALSE)

# A failing normalization must not disturb an already-published artifact. Because the script
# validates everything on a temp file and promotes it only after every check passes, a
# failing run leaves the existing final file byte-for-byte (and by hash) unchanged.
set(preserve_output "${work}/preserve-output.qmltypes")
set(preserve_seed "${work}/preserve-seed.qmltypes")
set(preserve_input "${work}/preserve-input.qmltypes")
file(WRITE "${preserve_seed}" "${baseline}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${preserve_seed}" "-DOUTPUT=${preserve_output}" -P "${DVS_QMLTYPES_SCRIPT}"
    RESULT_VARIABLE preserve_seed_result
)
dvs_content_check("the seed run must publish a valid artifact" preserve_seed_result EQUAL 0)
file(SHA256 "${preserve_output}" preserve_hash_before)
file(WRITE "${preserve_input}" "${tail_truncated}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${preserve_input}" "-DOUTPUT=${preserve_output}" -P "${DVS_QMLTYPES_SCRIPT}"
    RESULT_VARIABLE preserve_result
)
dvs_content_check("the tail-truncated run against an existing artifact must fail" preserve_result GREATER 0)
file(SHA256 "${preserve_output}" preserve_hash_after)
dvs_content_check("a failing run must leave the existing artifact hash unchanged" preserve_hash_after STREQUAL preserve_hash_before)
file(READ "${preserve_output}" preserve_content_after)
string(FIND "${preserve_content_after}" "\"double\"" preserve_double)
dvs_content_check("the existing artifact must still be the normalized baseline" preserve_double GREATER -1)

if(NOT case_count EQUAL expected_case_count)
    message(FATAL_ERROR "Executed ${case_count} contract cases, expected ${expected_case_count}.")
endif()
if(NOT content_check_count EQUAL expected_content_check_count)
    message(FATAL_ERROR "Executed ${content_check_count} content checks, expected ${expected_content_check_count}.")
endif()
message(STATUS "QmlTypes contract: ${case_count} cases and ${content_check_count} content checks behaved as expected.")
