include_guard(GLOBAL)

function(_dvs_add_hlsl_header_compiler)
    if(TARGET dvs_hlsl_header_compiler)
        return()
    endif()

    add_executable(
        dvs_hlsl_header_compiler
        "${PROJECT_SOURCE_DIR}/src/platform_windows/tools/HlslHeaderCompiler.cpp"
    )
    target_link_libraries(dvs_hlsl_header_compiler PRIVATE d3dcompiler dxguid)
    dvs_apply_project_warnings(dvs_hlsl_header_compiler)
    set_target_properties(dvs_hlsl_header_compiler PROPERTIES FOLDER "Build Tools")
endfunction()

function(dvs_compile_nv12_shaders)
    set(oneValueArguments OUTPUT_HEADER_VARIABLE OUTPUT_INCLUDE_DIRECTORY_VARIABLE)
    cmake_parse_arguments(PARSE_ARGV 0 argument "" "${oneValueArguments}" "")

    if(argument_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "dvs_compile_nv12_shaders received unknown arguments: "
            "${argument_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT argument_OUTPUT_HEADER_VARIABLE)
        message(FATAL_ERROR
            "dvs_compile_nv12_shaders requires OUTPUT_HEADER_VARIABLE.")
    endif()
    if(NOT argument_OUTPUT_INCLUDE_DIRECTORY_VARIABLE)
        message(FATAL_ERROR
            "dvs_compile_nv12_shaders requires OUTPUT_INCLUDE_DIRECTORY_VARIABLE.")
    endif()

    _dvs_add_hlsl_header_compiler()

    set(shaderDirectory "${PROJECT_SOURCE_DIR}/src/ui_qml/shaders")
    set(composeShader "${shaderDirectory}/Compose.hlsl")
    set(nv12Shader "${shaderDirectory}/Nv12ToRgb.hlsl")
    set(probeShader "${shaderDirectory}/SourcePixelProbe.hlsl")
    set(generatedIncludeDirectory "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
    set(generatedHeader
        "${generatedIncludeDirectory}/dvs/platform/shaders/DvsNv12Shaders.generated.h")

    add_custom_command(
        OUTPUT "${generatedHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${generatedIncludeDirectory}/dvs/platform/shaders"
        COMMAND "$<TARGET_FILE:dvs_hlsl_header_compiler>"
                --compose "${composeShader}"
                --nv12 "${nv12Shader}"
                --probe "${probeShader}"
                --output "${generatedHeader}"
        DEPENDS
            dvs_hlsl_header_compiler
            "${composeShader}"
            "${nv12Shader}"
            "${probeShader}"
        COMMENT "Compiling deterministic D3D11 NV12 shader bytecode"
        VERBATIM
    )
    set_source_files_properties("${generatedHeader}" PROPERTIES GENERATED TRUE)

    set(${argument_OUTPUT_HEADER_VARIABLE} "${generatedHeader}" PARENT_SCOPE)
    set(${argument_OUTPUT_INCLUDE_DIRECTORY_VARIABLE}
        "${generatedIncludeDirectory}"
        PARENT_SCOPE
    )
endfunction()
