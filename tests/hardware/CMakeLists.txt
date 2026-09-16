if(DEFINED DVS_POWERSHELL_EXECUTABLE AND
   NOT EXISTS "${DVS_POWERSHELL_EXECUTABLE}")
    unset(DVS_POWERSHELL_EXECUTABLE CACHE)
endif()

find_program(DVS_POWERSHELL_EXECUTABLE NAMES pwsh powershell REQUIRED)

dvs_add_test(
    NAME hardware.d3d11va_decode
    LAYER hardware
    MODULE media_ffmpeg
    COMMAND
        $<TARGET_FILE:dvs_media_component_tests>
        --gtest_filter=SoftwareDecoderHardwareTests.*
)
set_tests_properties(hardware.d3d11va_decode PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)

dvs_add_test(
    NAME hardware.d3d11_zero_copy
    LAYER hardware
    MODULE platform_windows
    COMMAND
        $<TARGET_FILE:dvs_graphics_device_broker_component_tests>
        --gtest_filter=GpuTransferActorTests.PublishesDecoderOwnedNv12ArraySlicesWithoutACopy
)
set_tests_properties(hardware.d3d11_zero_copy PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)

# These execute through the real QML controls on the GPU runner. Unlike the regular source-count
# performance profiles, they capture the resulting Wipe/Diff surface and reject a black output.
foreach(dvs_comparison_mode IN ITEMS wipe diff)
    if(dvs_comparison_mode STREQUAL "diff")
        set(dvs_comparison_profile 1080p60-2source-diff)
    else()
        set(dvs_comparison_profile 1080p60-2source)
    endif()
    dvs_add_test(
        NAME hardware.${dvs_comparison_mode}-2source
        LAYER hardware
        MODULE app
        COMMAND
            "${DVS_POWERSHELL_EXECUTABLE}"
            -NoLogo
            -NoProfile
            -ExecutionPolicy
            Bypass
            -File
            "${PROJECT_SOURCE_DIR}/tools/testing/run-performance-gate.ps1"
            -Profile
            "${dvs_comparison_profile}"
            -ComparisonMode
            "${dvs_comparison_mode}"
            -Executable
            $<TARGET_FILE:VCStation>
            -DurationSeconds
            300
    )
    set_tests_properties(
        hardware.${dvs_comparison_mode}-2source
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 480
    )
endforeach()

# Mode changes must not drop the already presented canonical frame. The performance entry point
# starts in Side, switches through the real Wipe control, and captures that retained frame.
dvs_add_test(
    NAME hardware.mode-switch-retained-frame
    LAYER hardware
    MODULE app
    COMMAND
        "${DVS_POWERSHELL_EXECUTABLE}"
        -NoLogo
        -NoProfile
        -ExecutionPolicy
        Bypass
        -File
        "${PROJECT_SOURCE_DIR}/tools/testing/run-performance-gate.ps1"
        -Profile
        1080p60-2source
        -ComparisonMode
        wipe
        -RequireRetainedFrame
        -RunName
        1080p60-2source-mode-switch-wipe
        -Executable
        $<TARGET_FILE:VCStation>
        -DurationSeconds
        300
)
set_tests_properties(
    hardware.mode-switch-retained-frame
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 480
)

# This fixture carries a 90-degree presentation transform. D3D11VA supplies aligned decoder
# textures, so the visible Wipe capture covers both rotation and decoder padding on the GPU path.
dvs_add_test(
    NAME hardware.rotated-padded-wipe
    LAYER hardware
    MODULE app
    COMMAND
        "${DVS_POWERSHELL_EXECUTABLE}"
        -NoLogo
        -NoProfile
        -ExecutionPolicy
        Bypass
        -File
        "${PROJECT_SOURCE_DIR}/tools/testing/run-performance-gate.ps1"
        -Profile
        1080p60-2source-rotated
        -ComparisonMode
        wipe
        -RequireRetainedFrame
        -Executable
        $<TARGET_FILE:VCStation>
        -DurationSeconds
        300
)
set_tests_properties(
    hardware.rotated-padded-wipe
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 480
)

dvs_add_test(
    NAME performance.1080p60-1source
    LAYER performance
    MODULE app
    COMMAND
        "${DVS_POWERSHELL_EXECUTABLE}"
        -NoLogo
        -NoProfile
        -ExecutionPolicy
        Bypass
        -File
        "${PROJECT_SOURCE_DIR}/tools/testing/run-performance-gate.ps1"
        -Profile
        1080p60-1source
        -Executable
        $<TARGET_FILE:VCStation>
        -DurationSeconds
        300
)
set_tests_properties(
    performance.1080p60-1source
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 480
)

dvs_add_test(
    NAME performance.1080p60-2source
    LAYER performance
    MODULE app
    COMMAND
        "${DVS_POWERSHELL_EXECUTABLE}"
        -NoLogo
        -NoProfile
        -ExecutionPolicy
        Bypass
        -File
        "${PROJECT_SOURCE_DIR}/tools/testing/run-performance-gate.ps1"
        -Profile
        1080p60-2source
        -Executable
        $<TARGET_FILE:VCStation>
        -DurationSeconds
        300
)
set_tests_properties(
    performance.1080p60-2source
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 480
)

dvs_add_test(
    NAME performance.1080p60
    LAYER performance
    MODULE app
    COMMAND
        "${DVS_POWERSHELL_EXECUTABLE}"
        -NoLogo
        -NoProfile
        -ExecutionPolicy
        Bypass
        -File
        "${PROJECT_SOURCE_DIR}/tools/testing/run-performance-gate.ps1"
        -Profile
        1080p60
        -Executable
        $<TARGET_FILE:VCStation>
        -DurationSeconds
        300
)
set_tests_properties(
    performance.1080p60
    PROPERTIES
        RUN_SERIAL TRUE
        TIMEOUT 480
)

foreach(profile IN ITEMS 1080p120-1source 1080p120-2source 1080p120-3source)
    dvs_add_test(
        NAME performance.${profile}
        LAYER performance
        MODULE app
        COMMAND
            "${DVS_POWERSHELL_EXECUTABLE}"
            -NoLogo
            -NoProfile
            -ExecutionPolicy
            Bypass
            -File
            "${PROJECT_SOURCE_DIR}/tools/testing/run-performance-gate.ps1"
            -Profile
            "${profile}"
            -Executable
            $<TARGET_FILE:VCStation>
            -DurationSeconds
            60
    )
    set_tests_properties(
        performance.${profile}
        PROPERTIES
            RUN_SERIAL TRUE
            TIMEOUT 210
    )
endforeach()
