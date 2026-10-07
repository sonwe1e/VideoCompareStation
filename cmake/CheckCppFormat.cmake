# Format every C++ file named in FILE_LIST with clang-format. Check mode (default) fails when
# any file differs from clang-format 19.1.5 output; DVS_FORMAT_APPLY=ON rewrites files in place
# for the `format` target. The per-file loop keeps each CreateProcess call short: handing the
# whole list to one clang-format invocation exceeded the Windows 32,767-character command-line
# limit on deep checkouts and the target failed before formatting anything.
if(NOT DEFINED CLANG_FORMAT_EXECUTABLE OR NOT DEFINED FILE_LIST)
    message(FATAL_ERROR "CLANG_FORMAT_EXECUTABLE and FILE_LIST are required.")
endif()

file(STRINGS "${FILE_LIST}" cppFormatFiles)
if(cppFormatFiles STREQUAL "")
    message(FATAL_ERROR "The C++ format file list is empty; refusing a format gate that checks nothing.")
endif()

set(formattedFiles 0)
foreach(cppFile IN LISTS cppFormatFiles)
    if(DVS_FORMAT_APPLY)
        execute_process(
            COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i "${cppFile}"
            RESULT_VARIABLE result
            ERROR_VARIABLE diagnostics
            ENCODING UTF-8
        )
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "clang-format failed for ${cppFile}: ${diagnostics}")
        endif()
    else()
        execute_process(
            COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror "${cppFile}"
            RESULT_VARIABLE result
            OUTPUT_QUIET
            ERROR_VARIABLE diagnostics
            ENCODING UTF-8
        )
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "clang-format check failed for ${cppFile}:\n${diagnostics}")
        endif()
    endif()
    math(EXPR formattedFiles "${formattedFiles} + 1")
endforeach()

message(STATUS "clang-format checked ${formattedFiles} C++ files.")
