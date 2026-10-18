# Validates a qmltyperegistrar-generated .qmltypes against the Dvs.Ui module contract and
# normalizes moc's C++ type spellings to the QML tooling spellings before the artifact is
# published to the build tree.
#
# Phase 1 - spelling normalization (in memory): moc records Q_PROPERTY types with C++
#   spellings ("qreal", "qint64", "quint64"); QML tooling declares them as "double",
#   "qlonglong", "qulonglong" and cannot resolve the C++ forms. The rewrite does not require
#   any replacement to happen: a future generator that emits the QML spellings directly
#   passes unchanged. Residual known-unresolvable spellings fail the run.
# Phase 2 - structural validation: the artifact is checked as a whole (the tooling import,
#   a single Module wrapper, braces that balance and close the module, a non-truncated tail)
#   and every Component block is cut out by real brace matching, so a tail that lost its
#   final "}" - or any unbalanced truncation - fails instead of publishing a half-file.
# Phase 3 - contract validation on the real component blocks: each component exports into
#   Dvs.Ui at 1.0 under its own name (the export identity, not the C++ class name), the two
#   composition-root-owned controllers are uncreatable while the other three stay creatable,
#   and the pilot-critical members exist inside their own block.
# Phase 4 - publish only after every check passed: the validated content is written to a
#   temporary file, read back and compared byte-for-byte, and only then does it replace the
#   final path. A failing check aborts before the final path is touched, so an already
#   published artifact keeps its content and hash.
#   Every check is executed and counted; the script asserts the expected count, so a missing
#   anchor cannot turn checks into silent passes.
#
# Inputs: INPUT (generated .qmltypes), OUTPUT (normalized .qmltypes).

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "NormalizeQmlTypes requires -DINPUT=<file> and -DOUTPUT=<file>.")
endif()
if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Generated qmltypes does not exist: ${INPUT}")
endif()

set(dvs_check_count 0)
set(dvs_expected_check_count 38)

macro(dvs_require description)
    math(EXPR dvs_check_count "${dvs_check_count} + 1")
    if(NOT (${ARGN}))
        message(FATAL_ERROR "Dvs.Ui qmltypes contract failed: ${description} (${INPUT})")
    endif()
endmacro()

file(READ "${INPUT}" content)

set(
    dvs_expected_components
    ColorClipboardController
    ComparisonSurface
    HoldKeyHandler
    ImageReviewController
    JogKeyHandler
)

# --- Phase 1: spelling normalization ------------------------------------------------------

set(dvs_spelling_map "qreal=double" "qint64=qlonglong" "quint64=qulonglong")
foreach(mapping IN LISTS dvs_spelling_map)
    string(REPLACE "=" ";" mapping_parts "${mapping}")
    list(GET mapping_parts 0 cpp_spelling)
    list(GET mapping_parts 1 qml_spelling)
    string(REGEX MATCHALL "\"${cpp_spelling}\"" occurrences "${content}")
    list(LENGTH occurrences rewrite_count)
    message(STATUS "NormalizeQmlTypes: rewrote ${rewrite_count} \"${cpp_spelling}\" spelling(s) to \"${qml_spelling}\"")
    string(REPLACE "\"${cpp_spelling}\"" "\"${qml_spelling}\"" content "${content}")
endforeach()

# --- Phase 2: structural validation -------------------------------------------------------

# Whole-file anchors: the tooling import header and exactly one Module wrapper.
string(FIND "${content}" "import QtQuick.tooling" dvs_import_anchor)
dvs_require("the artifact must declare the QtQuick.tooling import" dvs_import_anchor GREATER -1)
string(REGEX MATCHALL "Module [{]" dvs_module_open "${content}")
list(LENGTH dvs_module_open dvs_module_open_count)
dvs_require("the artifact must contain exactly one Module wrapper" dvs_module_open_count EQUAL 1)

# Cut each Component block out by real brace matching. A line is a safe unit for brace
# accounting: a member/signal/method line may contain ';' but never a bare newline inside a
# token, and splitting the match on ';' only inserts newlines - it never changes the '{'/'}'
# totals - so the depth walk and the name/export/member substrings below stay correct.
string(REGEX MATCHALL "[^\r\n]+" dvs_lines "${content}")
set(dvs_depth 0)
set(dvs_blocks)
set(dvs_block "")
set(dvs_in_component FALSE)
foreach(dvs_line IN LISTS dvs_lines)
    string(REGEX MATCHALL "[{]" dvs_line_open "${dvs_line}")
    list(LENGTH dvs_line_open dvs_line_open_count)
    string(REGEX MATCHALL "[}]" dvs_line_close "${dvs_line}")
    list(LENGTH dvs_line_close dvs_line_close_count)
    if(NOT dvs_in_component AND dvs_depth EQUAL 1 AND dvs_line MATCHES "^ *Component [{]$")
        set(dvs_in_component TRUE)
        set(dvs_block "")
    endif()
    if(dvs_in_component)
        string(APPEND dvs_block "${dvs_line}\n")
    endif()
    math(EXPR dvs_depth "${dvs_depth} + ${dvs_line_open_count} - ${dvs_line_close_count}")
    if(dvs_in_component AND dvs_depth EQUAL 1)
        list(APPEND dvs_blocks "${dvs_block}")
        set(dvs_in_component FALSE)
    endif()
endforeach()

# Braces that balance and close the module: a tail that lost its final "}" (or any
# unbalanced truncation) leaves the depth non-zero, and a block cut mid-way leaves one
# dangling. Both fail here instead of publishing a half-file.
dvs_require("the module braces must balance and close (no truncated tail)" dvs_depth EQUAL 0)
dvs_require("no component block may be left dangling" dvs_in_component STREQUAL "FALSE")

# An explicit tail check: the last non-whitespace character must be a closing brace.
string(REGEX REPLACE "[ \t\r\n]+$" "" dvs_content_trimmed "${content}")
string(LENGTH "${dvs_content_trimmed}" dvs_trimmed_length)
math(EXPR dvs_last_index "${dvs_trimmed_length} - 1")
string(SUBSTRING "${dvs_content_trimmed}" ${dvs_last_index} 1 dvs_last_char)
dvs_require("the artifact must end with a closing brace, not a truncated tail" dvs_last_char STREQUAL "}")

# Exactly five component blocks and exactly five component name lines. A duplicated name
# inside a block is caught by the name-line count; a stray or missing block by the block
# count. (A rename that stays a single well-formed block is caught by the export contract.)
list(LENGTH dvs_blocks dvs_block_count)
dvs_require("the module must describe exactly its five components" dvs_block_count EQUAL 5)
string(REGEX MATCHALL "name: \"dvs::ui::[A-Za-z0-9]+\"" dvs_name_lines "${content}")
list(LENGTH dvs_name_lines dvs_name_line_count)
dvs_require("the module must contain exactly five component name lines" dvs_name_line_count EQUAL 5)

# Map each block to its C++ name. The per-component blocks start empty so a missing or
# renamed component is reported by the export contract below instead of dereferencing an
# undefined variable.
foreach(component IN LISTS dvs_expected_components)
    set(dvs_block_${component} "")
endforeach()
foreach(dvs_block IN LISTS dvs_blocks)
    string(REGEX MATCH "name: \"dvs::ui::[A-Za-z0-9]+\"" dvs_name_match "${dvs_block}")
    string(REGEX REPLACE "name: \"dvs::ui::" "" dvs_component_name "${dvs_name_match}")
    string(REGEX REPLACE "\"" "" dvs_component_name "${dvs_component_name}")
    set(dvs_block_${dvs_component_name} "${dvs_block}")
endforeach()

# --- Phase 3: per-component contract checks (real block boundaries) -----------------------

# Each component exports into Dvs.Ui at 1.0 under its own name. The export identity is read
# from the exports line, so a rename that touches only the export (not the C++ class name),
# or a dropped/missing component, fails here.
foreach(component IN LISTS dvs_expected_components)
    string(FIND "${dvs_block_${component}}" "exports: [\"Dvs.Ui/${component} 1.0\"]" dvs_export_anchor)
    dvs_require("${component} must export as Dvs.Ui/${component} 1.0" dvs_export_anchor GREATER -1)
endforeach()

# The two controllers are uncreatable; the other three stay creatable.
foreach(component IN LISTS dvs_expected_components)
    if(component STREQUAL "ImageReviewController" OR component STREQUAL "ColorClipboardController")
        string(FIND "${dvs_block_${component}}" "isCreatable: false" dvs_uncreatable)
        dvs_require("${component} must be uncreatable" dvs_uncreatable GREATER -1)
    else()
        string(FIND "${dvs_block_${component}}" "isCreatable: false" dvs_uncreatable)
        dvs_require("${component} must stay creatable" dvs_uncreatable EQUAL -1)
    endif()
endforeach()

# The pilot-critical members, each found inside its own component's block.
set(dvs_image_review_members
    numericFormat
    frozenProbeSample
    channelText
    channelText16
    probeColorText
    captureProbeSample
    NumericFormat
    ProbeColorFormat
)
foreach(member IN LISTS dvs_image_review_members)
    string(FIND "${dvs_block_ImageReviewController}" "name: \"${member}\"" dvs_definition)
    dvs_require("ImageReviewController must define ${member}" dvs_definition GREATER -1)
endforeach()

string(FIND "${dvs_block_ColorClipboardController}" "name: \"copyText\"" dvs_copy_definition)
dvs_require("ColorClipboardController must define copyText" dvs_copy_definition GREATER -1)
string(FIND "${dvs_block_ColorClipboardController}" "name: \"statusText\"" dvs_status_definition)
dvs_require("ColorClipboardController must define statusText" dvs_status_definition GREATER -1)

string(FIND "${dvs_block_ComparisonSurface}" "name: \"ViewMode\"" dvs_view_mode)
dvs_require("ComparisonSurface must define the ViewMode enum" dvs_view_mode GREATER -1)

# Known C++ spellings that qmllint cannot resolve must not survive normalization. The list
# covers the spellings moc is known to emit for fixed-width integers; anything else surfaces
# through dvs_lint_qml on the QML files that use the module.
set(dvs_unresolvable_spellings qreal qint64 quint64 qint8 qint16 qint32 quint8 quint16 quint32)
foreach(spelling IN LISTS dvs_unresolvable_spellings)
    string(FIND "${content}" "\"${spelling}\"" dvs_leftover)
    dvs_require("no unresolvable C++ spelling \"${spelling}\" may remain" dvs_leftover EQUAL -1)
endforeach()

# --- Phase 4: publish to a temp file, read it back, then promote --------------------------

# The validated content is written to a temporary file first and read back before the final
# path is touched, so a stale scratch copy or a short write can never be published.
set(dvs_publish_temp "${OUTPUT}.normalize.tmp")
file(WRITE "${dvs_publish_temp}" "${content}")
file(READ "${dvs_publish_temp}" dvs_published)
dvs_require("the written artifact must match the validated content exactly" dvs_published STREQUAL content)

if(NOT dvs_check_count EQUAL dvs_expected_check_count)
    message(FATAL_ERROR
        "NormalizeQmlTypes executed ${dvs_check_count} checks, expected ${dvs_expected_check_count}; "
        "the contract script and its count drifted apart (${INPUT}).")
endif()

# Every check passed: only now does the final path change. A failing check aborts before
# this point, so an already published artifact keeps its content and hash.
file(REMOVE "${OUTPUT}")
file(RENAME "${dvs_publish_temp}" "${OUTPUT}")
