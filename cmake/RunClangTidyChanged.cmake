# clang-tidy on exactly the working tree's changed lintable files. Companion to the
# whole-repository `lint` target (cmake/Quality.cmake); the file list passed in is the
# same lintable set (src/*.cpp, platform_windows excluded), so the two runs can never
# disagree about scope.
#
# Self-counting by contract: the script asserts that it executed as many tidy runs as it
# planned and that every git invocation succeeded, so a silently empty or mis-parsed list
# cannot turn into a green pass without the counts saying so. A legitimately empty
# selection (no changed .cpp in the lintable set) passes with both counts at zero.

if(NOT DEFINED CLANG_TIDY_EXECUTABLE OR NOT DEFINED BINARY_DIR OR NOT DEFINED FILE_LIST
    OR NOT DEFINED GIT_EXECUTABLE OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR
        "RunClangTidyChanged.cmake requires CLANG_TIDY_EXECUTABLE, BINARY_DIR, FILE_LIST, "
        "GIT_EXECUTABLE and SOURCE_DIR.")
endif()

file(STRINGS "${FILE_LIST}" dvs_lintable_files)
list(REMOVE_DUPLICATES dvs_lintable_files)

# git emits forward slashes on every platform; cmake globs also use forward slashes here.
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" diff --name-only --diff-filter=ACMR
    OUTPUT_VARIABLE dvs_diff_output
    RESULT_VARIABLE dvs_diff_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT dvs_diff_result EQUAL 0)
    message(FATAL_ERROR "git diff failed (exit ${dvs_diff_result}); cannot select changed files.")
endif()
execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${SOURCE_DIR}" ls-files --others --exclude-standard
    OUTPUT_VARIABLE dvs_untracked_output
    RESULT_VARIABLE dvs_untracked_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT dvs_untracked_result EQUAL 0)
    message(FATAL_ERROR "git ls-files failed (exit ${dvs_untracked_result}); cannot select changed files.")
endif()

set(dvs_changed_files ${dvs_diff_output} ${dvs_untracked_output})
separate_arguments(dvs_changed_files NATIVE_COMMAND "${dvs_changed_files}")
list(REMOVE_DUPLICATES dvs_changed_files)

set(dvs_planned 0)
set(dvs_planned_list "")
# git prints repository-relative paths while the generated file list carries absolute
# paths; normalize the git side against SOURCE_DIR before the membership test, or every
# selection silently comes back empty (the self-count alone cannot catch that shape).
foreach(dvs_changed IN LISTS dvs_changed_files)
    if(dvs_changed MATCHES "\\.(cpp|cc|cxx)$")
        cmake_path(SET dvs_changed_abs NORMALIZE "${SOURCE_DIR}/${dvs_changed}")
        if(dvs_changed_abs IN_LIST dvs_lintable_files)
            list(APPEND dvs_planned_list "${dvs_changed_abs}")
            math(EXPR dvs_planned "${dvs_planned} + 1")
        endif()
    endif()
endforeach()

if(dvs_planned EQUAL 0)
    message(STATUS "lint-changed: 0 changed lintable C++ files "
        "(header-only or non-lintable change; the full lint target stays authoritative)")
    return()
endif()

set(dvs_executed 0)
set(dvs_failed "")
foreach(dvs_file IN LISTS dvs_planned_list)
    message(STATUS "lint-changed [${dvs_executed}/${dvs_planned}]: ${dvs_file}")
    execute_process(
        COMMAND "${CLANG_TIDY_EXECUTABLE}" "-p=${BINARY_DIR}"
            "--extra-arg=-Wno-unused-command-line-argument" "${dvs_file}"
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE dvs_tidy_result
    )
    math(EXPR dvs_executed "${dvs_executed} + 1")
    if(NOT dvs_tidy_result EQUAL 0)
        list(APPEND dvs_failed "${dvs_file} (exit ${dvs_tidy_result})")
    endif()
endforeach()

if(NOT dvs_executed EQUAL dvs_planned)
    message(FATAL_ERROR "lint-changed count mismatch: planned ${dvs_planned}, executed ${dvs_executed}.")
endif()
list(LENGTH dvs_failed dvs_failed_count)
if(dvs_failed_count GREATER 0)
    message(FATAL_ERROR "lint-changed: ${dvs_failed_count}/${dvs_executed} files failed "
        "clang-tidy:\n  ${dvs_failed}")
endif()
message(STATUS "lint-changed: ${dvs_executed}/${dvs_planned} files clean "
    "(counts asserted; full lint remains the hand-off gate)")
