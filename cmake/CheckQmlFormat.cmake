if(NOT DEFINED QMLFORMAT_EXECUTABLE OR NOT DEFINED FILE_LIST)
    message(FATAL_ERROR "QMLFORMAT_EXECUTABLE and FILE_LIST are required.")
endif()

file(STRINGS "${FILE_LIST}" qmlFiles)
foreach(qmlFile IN LISTS qmlFiles)
    execute_process(
        COMMAND "${QMLFORMAT_EXECUTABLE}" -w 4 "${qmlFile}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE formatted
        ERROR_VARIABLE diagnostics
        ENCODING UTF-8
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "qmlformat failed for ${qmlFile}: ${diagnostics}")
    endif()

    file(READ "${qmlFile}" source)

    # qmlformat writes the platform's native line ending, so on Windows its output is CRLF even
    # though the repository (and every checked-out .qml, per .gitattributes) is LF. Comparing the
    # raw bytes then reports a difference on every single file, which makes this gate unable to
    # pass on the platform the product ships on. Normalize both sides to LF and compare content;
    # a genuine formatting difference still fails, exactly as before.
    string(REPLACE "\r\n" "\n" formatted "${formatted}")
    string(REPLACE "\r\n" "\n" source "${source}")
    if(NOT source STREQUAL formatted)
        message(FATAL_ERROR "QML formatting differs: ${qmlFile}")
    endif()
endforeach()
