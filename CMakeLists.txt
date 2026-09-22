cmake_minimum_required(VERSION 4.4 FATAL_ERROR)

project(CompareStation VERSION 1.6.0 LANGUAGES CXX)

option(DVS_BUILD_DESKTOP "Build the Windows desktop application and packaging" ON)
option(DVS_BUILD_ADAPTERS "Build Windows, FFmpeg, and JSON adapters" ON)
option(DVS_BUILD_UI "Build Qt view models and QML UI" ON)
option(DVS_ENABLE_QUALITY_TOOLS "Configure clang-format, clang-tidy, and qmllint targets" ON)

if(DVS_BUILD_DESKTOP AND (NOT DVS_BUILD_ADAPTERS OR NOT DVS_BUILD_UI))
    message(FATAL_ERROR "DVS_BUILD_DESKTOP requires DVS_BUILD_ADAPTERS and DVS_BUILD_UI.")
endif()

if((DVS_BUILD_DESKTOP OR DVS_BUILD_ADAPTERS OR DVS_BUILD_UI) AND NOT WIN32)
    message(FATAL_ERROR "CompareStation supports Windows only.")
endif()

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Use the single-config Ninja generator through CMakePresets.json.")
endif()

if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Multi-config generators are not supported.")
endif()

if((DVS_BUILD_DESKTOP OR DVS_BUILD_ADAPTERS OR DVS_BUILD_UI) AND NOT MSVC)
    message(FATAL_ERROR "CompareStation requires the MSVC 2022 compiler.")
endif()

if((DVS_BUILD_DESKTOP OR DVS_BUILD_ADAPTERS OR DVS_BUILD_UI) AND
   NOT MSVC_TOOLSET_VERSION STREQUAL "143")
    message(FATAL_ERROR "CompareStation requires the Visual Studio 2022 v143 toolset.")
endif()

if(DVS_BUILD_DESKTOP OR DVS_BUILD_ADAPTERS OR DVS_BUILD_UI)
    get_filename_component(_dvs_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_dvs_expected_archiver "${_dvs_msvc_bin_dir}/lib.exe")
    cmake_path(NORMAL_PATH _dvs_expected_archiver)
    cmake_path(NORMAL_PATH CMAKE_AR OUTPUT_VARIABLE _dvs_configured_archiver)
    if(NOT _dvs_configured_archiver STREQUAL _dvs_expected_archiver)
        message(FATAL_ERROR
            "CompareStation requires the MSVC archiver '${_dvs_expected_archiver}', but CMAKE_AR is "
            "'${CMAKE_AR}'. Run CMake from a Visual Studio developer shell with "
            "'cmake --fresh --preset dev'.")
    endif()
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "CompareStation requires a 64-bit compiler environment.")
endif()

if((DVS_BUILD_DESKTOP OR DVS_BUILD_ADAPTERS OR DVS_BUILD_UI) AND
   NOT VCPKG_TARGET_TRIPLET STREQUAL "x64-windows")
    message(FATAL_ERROR "VCPKG_TARGET_TRIPLET must be x64-windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(DVS_SHELL_BINARY_BASENAME
    "CompareStationShell-${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    CACHE INTERNAL "Versioned CompareStation Explorer shell binary base name")
set(DVS_SHELL_BINARY_NAME
    "${DVS_SHELL_BINARY_BASENAME}.dll"
    CACHE INTERNAL "Versioned CompareStation Explorer shell binary file name")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/release-metadata.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/release-metadata.json"
    @ONLY
)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(DVS_ENABLE_ASAN "Enable MSVC AddressSanitizer for project targets" OFF)
option(DVS_ENABLE_COVERAGE "Enable native MSVC coverage instrumentation" OFF)
set(
    DVS_PREVIOUS_RELEASE_1_2_MSI
    "$ENV{DVS_PREVIOUS_RELEASE_1_2_MSI}"
    CACHE FILEPATH
    "Optional CompareStation 1.2.0 MSI used by the real packaged upgrade test"
)

if(DVS_ENABLE_ASAN AND DVS_ENABLE_COVERAGE)
    message(FATAL_ERROR "AddressSanitizer and coverage instrumentation are separate presets.")
endif()

if((DVS_ENABLE_ASAN OR DVS_ENABLE_COVERAGE) AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "AddressSanitizer and coverage instrumentation require Debug builds.")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Testing)
include(Dependencies)
include(Coverage)
include(Warnings)
include(Architecture)
include(Quality)

dvs_resolve_dependencies()

add_subdirectory(src/domain)
add_subdirectory(src/application)
add_subdirectory(src/presentation_contract)
if(DVS_BUILD_ADAPTERS)
    add_subdirectory(src/platform_windows)
    add_subdirectory(src/media_ffmpeg)
    add_subdirectory(src/persistence_json)
endif()
if(DVS_BUILD_UI)
    add_subdirectory(src/ui_qml)
endif()
if(DVS_BUILD_DESKTOP)
    add_subdirectory(src/shell_windows)
    add_subdirectory(src/app)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

dvs_validate_architecture()
if(DVS_ENABLE_QUALITY_TOOLS)
    dvs_add_quality_targets()
endif()

if(DVS_BUILD_DESKTOP)
    include(Install)
    include(Packaging)
endif()
