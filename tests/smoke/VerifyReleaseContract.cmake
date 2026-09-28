foreach(required IN ITEMS DVS_SOURCE_DIR DVS_BINARY_DIR DVS_EXPECTED_VERSION DVS_EXPECTED_SHELL)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required.")
    endif()
endforeach()

file(READ "${DVS_BINARY_DIR}/release-metadata.json" metadata)
string(JSON metadata_version GET "${metadata}" version)
string(JSON metadata_signed GET "${metadata}" signed)
string(JSON metadata_projects GET "${metadata}" projectFiles)
string(JSON metadata_audio GET "${metadata}" audioPlayback)
string(JSON metadata_sources GET "${metadata}" maximumSourceCount)
string(JSON metadata_shell GET "${metadata}" shellBinaryName)

if(NOT "${metadata_version}" STREQUAL "${DVS_EXPECTED_VERSION}")
    message(FATAL_ERROR "Release metadata version is '${metadata_version}', expected '${DVS_EXPECTED_VERSION}'.")
endif()
if(NOT "${metadata_shell}" STREQUAL "${DVS_EXPECTED_SHELL}")
    message(FATAL_ERROR "Release metadata shell is '${metadata_shell}', expected '${DVS_EXPECTED_SHELL}'.")
endif()
if(metadata_signed OR metadata_projects OR metadata_audio)
    message(FATAL_ERROR "Release metadata must keep signing, project files, and audio disabled.")
endif()
if(NOT "${metadata_sources}" STREQUAL "3")
    message(FATAL_ERROR "Release metadata maximumSourceCount must be 3, got '${metadata_sources}'.")
endif()

set(release_notes "${DVS_SOURCE_DIR}/docs/releases/v${DVS_EXPECTED_VERSION}.md")
if(NOT EXISTS "${release_notes}")
    message(FATAL_ERROR "Release notes are missing: ${release_notes}")
endif()

file(READ "${DVS_SOURCE_DIR}/.github/workflows/release.yml" workflow)
# The version has to appear in the workflow itself, so the publish scope cannot name one build
# while the body belongs to another.
string(FIND "${workflow}" "v${DVS_EXPECTED_VERSION}" version_position)
if(version_position EQUAL -1)
    message(FATAL_ERROR "Release workflow is missing 'v${DVS_EXPECTED_VERSION}'.")
endif()
# The body path may be spelled out for this version or derived from the validated version. Both
# publish the notes of the tag being built, and the derived form is what keeps a later tag from
# republishing the previous version's notes - the defect v2.0.1 removed. A path that names neither
# this version nor the derived expression is a release that ships somebody else's notes.
set(body_path_literal "body_path: docs/releases/v${DVS_EXPECTED_VERSION}.md")
# The derived path is written the way the workflow writes it, with the dollar-brace expression
# escaped so CMake reads it as text rather than as a variable reference.
set(body_path_derived "body_path: docs/releases/v\${{ needs.validate-version.outputs.version }}.md")
set(body_path_accepted 0)
foreach(candidate IN ITEMS "${body_path_literal}" "${body_path_derived}")
    string(FIND "${workflow}" "${candidate}" body_position)
    if(NOT body_position EQUAL -1)
        set(body_path_accepted 1)
    endif()
endforeach()
if(NOT body_path_accepted)
    message(FATAL_ERROR
        "Release workflow body_path must be '${body_path_literal}' or '${body_path_derived}'.")
endif()

file(READ "${DVS_SOURCE_DIR}/README.md" readme)
foreach(required_text IN ITEMS
        "${DVS_EXPECTED_SHELL}"
        "1.2.0→${DVS_EXPECTED_VERSION}"
        "不解码或播放音频")
    string(FIND "${readme}" "${required_text}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "README release contract is missing '${required_text}'.")
    endif()
endforeach()

file(READ "${DVS_SOURCE_DIR}/vcpkg.json" manifest)
string(JSON manifest_version GET "${manifest}" version-semver)
if(NOT manifest_version STREQUAL DVS_EXPECTED_VERSION)
    message(FATAL_ERROR "vcpkg manifest version does not match the project version.")
endif()
