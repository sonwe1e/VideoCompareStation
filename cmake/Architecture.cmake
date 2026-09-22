include_guard(GLOBAL)

function(dvs_register_architecture_target target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot register missing architecture target: ${target}")
    endif()

    get_property(registered GLOBAL PROPERTY DVS_ARCHITECTURE_TARGETS)
    if(target IN_LIST registered)
        message(FATAL_ERROR "Architecture target registered more than once: ${target}")
    endif()

    set_property(GLOBAL APPEND PROPERTY DVS_ARCHITECTURE_TARGETS "${target}")
endfunction()

function(_dvs_allowed_dependencies target outputVariable)
    if(target STREQUAL "dvs_domain")
        set(allowed)
    elseif(target STREQUAL "dvs_application")
        set(allowed dvs_domain)
    elseif(target STREQUAL "dvs_presentation_contract")
        set(allowed)
    elseif(target STREQUAL "dvs_windows_support")
        set(allowed dvs_application)
    elseif(target STREQUAL "dvs_graphics_d3d11")
        set(allowed dvs_application dvs_presentation_contract)
    elseif(target STREQUAL "dvs_platform_windows")
        set(allowed dvs_windows_support dvs_graphics_d3d11)
    elseif(target STREQUAL "dvs_ui_models")
        set(allowed dvs_application dvs_presentation_contract)
    elseif(target STREQUAL "dvs_ui_d3d11_bridge")
        set(
            allowed
            dvs_ui_models
            dvs_graphics_d3d11
            dvs_presentation_contract
        )
    elseif(target STREQUAL "dvs_ui_qml")
        set(allowed dvs_ui_models dvs_ui_d3d11_bridge)
    elseif(target STREQUAL "dvs_media_ffmpeg")
        set(
            allowed
            dvs_application
            dvs_windows_support
            dvs_graphics_d3d11
        )
    elseif(target STREQUAL "dvs_persistence_json")
        set(allowed dvs_application dvs_windows_support)
    elseif(target STREQUAL "CompareStationShell")
        set(allowed)
    elseif(target MATCHES "^CompareStation(Cli)?$")
        set(
            allowed
            dvs_platform_windows
            dvs_windows_support
            dvs_graphics_d3d11
            dvs_media_ffmpeg
            dvs_persistence_json
            dvs_ui_qml
            dvs_ui_models
            dvs_ui_d3d11_bridge
        )
    else()
        message(FATAL_ERROR "No architecture rule exists for target: ${target}")
    endif()

    set(${outputVariable} "${allowed}" PARENT_SCOPE)
endfunction()

function(_dvs_reject_forbidden_core_dependency target dependency)
    if(target MATCHES "^dvs_(domain|application)$" AND
       dependency MATCHES
       "(Qt[0-9]+::|PkgConfig::DVS_FFMPEG|nlohmann_json::|GTest::|dvs(::|_)(qt|ffmpeg|json|test)_dependencies|(^|[^A-Za-z0-9_])(d3d11|dxgi|d2d1|user32|shell32)([^A-Za-z0-9_]|$))")
        message(
            FATAL_ERROR
            "Framework/native dependency is forbidden in ${target}: ${dependency}"
        )
    endif()
endfunction()

function(dvs_validate_architecture)
    get_property(registered GLOBAL PROPERTY DVS_ARCHITECTURE_TARGETS)
    if(NOT registered)
        message(FATAL_ERROR "No production targets were registered for architecture validation.")
    endif()

    foreach(target IN LISTS registered)
        _dvs_allowed_dependencies("${target}" allowed)
        get_target_property(linkLibraries "${target}" LINK_LIBRARIES)
        if(linkLibraries STREQUAL "linkLibraries-NOTFOUND")
            set(linkLibraries)
        endif()

        foreach(dependency IN LISTS linkLibraries)
            _dvs_reject_forbidden_core_dependency("${target}" "${dependency}")
            if(dependency MATCHES "^\\$<")
                foreach(candidate IN LISTS registered)
                    if(dependency MATCHES "(^|[^A-Za-z0-9_])${candidate}([^A-Za-z0-9_]|$)")
                        message(
                            FATAL_ERROR
                            "${target} hides project dependency ${candidate} in a generator expression. "
                            "Project dependency edges must be explicit."
                        )
                    endif()
                endforeach()
                continue()
            endif()

            set(resolvedDependency "${dependency}")
            if(TARGET "${dependency}")
                get_target_property(aliasedTarget "${dependency}" ALIASED_TARGET)
                if(aliasedTarget)
                    set(resolvedDependency "${aliasedTarget}")
                endif()
            endif()
            _dvs_reject_forbidden_core_dependency("${target}" "${resolvedDependency}")

            if(resolvedDependency IN_LIST registered AND NOT resolvedDependency IN_LIST allowed)
                message(
                    FATAL_ERROR
                    "Forbidden dependency edge: ${target} -> ${resolvedDependency}. "
                    "See cmake/Architecture.cmake for the inward-only allow-list."
                )
            endif()
        endforeach()

        get_target_property(interfaceLibraries "${target}" INTERFACE_LINK_LIBRARIES)
        if(interfaceLibraries STREQUAL "interfaceLibraries-NOTFOUND")
            set(interfaceLibraries)
        endif()
        foreach(dependency IN LISTS interfaceLibraries)
            _dvs_reject_forbidden_core_dependency("${target}" "${dependency}")
            foreach(candidate IN LISTS registered)
                if(dependency MATCHES "(^|[^A-Za-z0-9_])${candidate}([^A-Za-z0-9_]|$)" AND
                   NOT candidate IN_LIST allowed)
                    message(
                        FATAL_ERROR
                        "Forbidden transitive dependency edge: ${target} -> ${candidate}."
                    )
                endif()
            endforeach()
        endforeach()
    endforeach()
endfunction()
