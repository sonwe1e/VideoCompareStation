include_guard(GLOBAL)

set(CPACK_PACKAGE_NAME "CompareStation")
set(CPACK_PACKAGE_VENDOR "CompareStation Contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "CompareStation for frame-exact multi-video review")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "CompareStation-${PROJECT_VERSION}-windows-x64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "CompareStation")
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_PRE_BUILD_SCRIPTS "${PROJECT_SOURCE_DIR}/cmake/VerifyPackageStage.cmake")

include(CPack)
