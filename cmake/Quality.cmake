include_guard(GLOBAL)

function(_dvs_add_missing_tool_target target toolName)
    add_custom_target(
        "${target}"
        COMMAND "${CMAKE_COMMAND}" -E echo "${toolName} is required for target ${target}."
        COMMAND "${CMAKE_COMMAND}" -E false
        VERBATIM
    )
endfunction()

function(_dvs_require_llvm_tool_version executable toolName)
    set(dvs_required_llvm_tool_version "19.1.5")
    execute_process(
        COMMAND "${executable}" --version
        RESULT_VARIABLE dvs_tool_version_result
        OUTPUT_VARIABLE dvs_tool_version_output
        ERROR_VARIABLE dvs_tool_version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT dvs_tool_version_result EQUAL 0)
        message(FATAL_ERROR
            "Could not determine ${toolName} version from '${executable}': ${dvs_tool_version_error}")
    endif()

    string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" dvs_tool_version "${dvs_tool_version_output}")
    if(NOT dvs_tool_version)
        message(FATAL_ERROR
            "Could not parse the ${toolName} version from '${dvs_tool_version_output}'.")
    endif()

    if(NOT "${dvs_tool_version}" STREQUAL "${dvs_required_llvm_tool_version}")
        message(FATAL_ERROR
            "VCStation requires ${toolName} ${dvs_required_llvm_tool_version}, but '${executable}' "
            "reports ${dvs_tool_version}.")
    endif()
endfunction()

function(dvs_add_quality_targets)
    add_custom_target(
        dvs_check_no_playback_qimage
        COMMAND
            "${CMAKE_COMMAND}"
            "-DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            -P
            "${PROJECT_SOURCE_DIR}/cmake/CheckNoPlaybackQImage.cmake"
        VERBATIM
    )

    set(dvs_quality_tool_hints)
    if(DEFINED ENV{VCToolsInstallDir})
        set(dvs_vc_tools_install_dir "$ENV{VCToolsInstallDir}")
        cmake_path(GET dvs_vc_tools_install_dir PARENT_PATH dvs_msvc_tools_dir)
        cmake_path(GET dvs_msvc_tools_dir PARENT_PATH dvs_vc_tools_dir)
        list(APPEND dvs_quality_tool_hints "${dvs_vc_tools_dir}/Llvm/x64/bin")
    endif()

    file(
        GLOB_RECURSE cppFormatFiles
        CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/src/*.h"
        "${PROJECT_SOURCE_DIR}/tests/*.cpp"
        "${PROJECT_SOURCE_DIR}/tests/*.h"
    )
    # Vendored third-party sources keep their upstream layout; they are excluded from the
    # formatting gate just like they are from clang-tidy's header filter.
    list(FILTER cppFormatFiles EXCLUDE REGEX "[/\\\\]third_party[/\\\\]")
    file(
        GLOB_RECURSE cppLintFiles
        CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
    )
    # clang-tidy's MSVC frontend does not reliably parse the Windows SDK headers used by the
    # platform adapter. Production adapters outside that boundary retain analyzer coverage;
    # platform code and all tests remain warning-fatal under the MSVC /W4 /WX build gate.
    list(FILTER cppLintFiles EXCLUDE REGEX "[/\\\\]platform_windows[/\\\\]")
    if(DVS_BUILD_UI)
        file(
            GLOB_RECURSE qmlFiles
            CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/src/ui_qml/qml/*.qml"
        )
    endif()
    list(SORT cppFormatFiles)
    list(SORT cppLintFiles)
    list(SORT qmlFiles)

    find_program(
        CLANG_FORMAT_EXECUTABLE
        NAMES clang-format
        HINTS ${dvs_quality_tool_hints}
        REQUIRED
    )
    find_program(
        CLANG_TIDY_EXECUTABLE
        NAMES clang-tidy
        HINTS ${dvs_quality_tool_hints}
        REQUIRED
    )
    _dvs_require_llvm_tool_version("${CLANG_FORMAT_EXECUTABLE}" "clang-format")
    _dvs_require_llvm_tool_version("${CLANG_TIDY_EXECUTABLE}" "clang-tidy")

    add_custom_target(
        dvs_format_cpp
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${cppFormatFiles}
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    add_custom_target(
        dvs_format_check_cpp
        COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${cppFormatFiles}
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
    # clang-tidy is designed to analyze one translation unit per process. Passing the complete
    # source list to one LLVM 19.1.5 process can retain analyzer state across translation units
    # and crash inside the MSVC standard-library model, hiding the actual lint result.
    set(dvs_clang_tidy_commands)
    foreach(dvs_cpp_lint_file IN LISTS cppLintFiles)
        list(
            APPEND
            dvs_clang_tidy_commands
            COMMAND
            "${CLANG_TIDY_EXECUTABLE}"
            "-p=${CMAKE_BINARY_DIR}"
            "--extra-arg=-Wno-unused-command-line-argument"
            "${dvs_cpp_lint_file}"
        )
    endforeach()
    add_custom_target(lint ${dvs_clang_tidy_commands} VERBATIM)

    add_custom_target(format DEPENDS dvs_format_cpp)
    add_custom_target(format-check DEPENDS dvs_format_check_cpp)
    add_dependencies(format-check dvs_check_no_playback_qimage)
    # MSVC compile commands generated by Ninja reference per-object module-map response files.
    # Those files do not exist in a fresh build tree until Ninja prepares the corresponding
    # production targets, so make the standalone lint command establish that prerequisite.
    add_dependencies(lint dvs_check_no_playback_qimage)
    if(DVS_BUILD_DESKTOP)
        add_dependencies(lint VCStation VCStationCli)
    else()
        add_dependencies(lint dvs_domain dvs_application dvs_presentation_contract)
    endif()

    if(BUILD_TESTING)
        add_test(
            NAME quality.no_playback_qimage
            COMMAND
                "${CMAKE_COMMAND}"
                "-DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
                -P
                "${PROJECT_SOURCE_DIR}/cmake/CheckNoPlaybackQImage.cmake"
        )
        set_tests_properties(
            quality.no_playback_qimage
            PROPERTIES LABELS "quality;architecture" TIMEOUT 10
        )
    endif()

    if(qmlFiles)
        set(qtToolHints)
        if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
            list(
                APPEND
                qtToolHints
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/Qt6/bin"
            )
        endif()

        find_program(QMLFORMAT_EXECUTABLE NAMES qmlformat HINTS ${qtToolHints} REQUIRED)
        find_program(QMLLINT_EXECUTABLE NAMES qmllint HINTS ${qtToolHints} REQUIRED)

        set(qmlFileList "${CMAKE_BINARY_DIR}/dvs-qml-format-files.txt")
        string(REPLACE ";" "\n" qmlFileListContents "${qmlFiles}")
        file(GENERATE OUTPUT "${qmlFileList}" CONTENT "${qmlFileListContents}\n")

        add_custom_target(
            dvs_format_qml
            COMMAND "${QMLFORMAT_EXECUTABLE}" -i -w 4 ${qmlFiles}
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
        add_custom_target(
            dvs_format_check_qml
            COMMAND
                "${CMAKE_COMMAND}"
                "-DQMLFORMAT_EXECUTABLE=${QMLFORMAT_EXECUTABLE}"
                "-DFILE_LIST=${qmlFileList}"
                -P
                "${PROJECT_SOURCE_DIR}/cmake/CheckQmlFormat.cmake"
            VERBATIM
        )
        add_custom_target(
            dvs_lint_qml
            COMMAND "${QMLLINT_EXECUTABLE}" --max-warnings 0 ${qmlFiles}
            COMMAND_EXPAND_LISTS
            VERBATIM
        )

        add_dependencies(format dvs_format_qml)
        add_dependencies(format-check dvs_format_check_qml)
        add_dependencies(lint dvs_lint_qml)
    endif()
endfunction()
