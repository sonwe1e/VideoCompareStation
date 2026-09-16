add_library(
    dvs_application
    STATIC
        src/Alignment.cpp
        src/AlignmentCacheIdentity.cpp
        src/AlignmentWorkEstimator.cpp
        src/AlignmentWorkflow.cpp
        src/ComparisonExactness.cpp
        src/CoordinatorPublication.cpp
        src/Module.cpp
        src/PlaybackCoordinator.cpp
        src/PlaybackTiming.cpp
        src/PlaybackTrace.cpp
        src/PrefetchScheduler.cpp
        src/SessionSnapshot.cpp
)
add_library(dvs::application ALIAS dvs_application)

target_include_directories(
    dvs_application
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(dvs_application PUBLIC dvs_domain)

# MSVC 14.44 can crash in its optimizer when this state-machine translation unit is compiled with
# /O2 /Ob2. Keep full optimization while limiting automatic inlining for this one source file.
if(MSVC)
    set_source_files_properties(src/PlaybackCoordinator.cpp PROPERTIES COMPILE_OPTIONS /Ob1)
endif()

dvs_apply_project_warnings(dvs_application)
dvs_register_architecture_target(dvs_application)
set_target_properties(dvs_application PROPERTIES FOLDER "Modules")
