include_guard(GLOBAL)

set(CPACK_PACKAGE_NAME "CompareStation")
set(CPACK_PACKAGE_VENDOR "CompareStation Contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "CompareStation for frame-exact multi-video review")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "CompareStation-${PROJECT_VERSION}-windows-x64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "CompareStation")
set(CPACK_WIX_PRODUCT_ICON "${PROJECT_SOURCE_DIR}/assets/branding/comparestation.ico")
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_WIX_VERSION 4)
set(CPACK_WIX_INSTALL_SCOPE perMachine)
set(CPACK_WIX_CUSTOM_XMLNS "ui=http://wixtoolset.org/schemas/v4/wxs/ui")
set(CPACK_WIX_UPGRADE_GUID "8E0E8272-2FA8-4B2D-A929-809E95D93DE2")
set(DVS_CPACK_WIX_CUSTOM_XMLNS_PLACEHOLDER "@CPACK_WIX_CUSTOM_XMLNS_EXPANDED@")
configure_file(
    "${PROJECT_SOURCE_DIR}/packaging/wix/WIX.template.in"
    "${PROJECT_BINARY_DIR}/packaging/WIX.template"
    @ONLY
)
set(CPACK_WIX_TEMPLATE "${PROJECT_BINARY_DIR}/packaging/WIX.template")
set(CPACK_PRE_BUILD_SCRIPTS "${PROJECT_SOURCE_DIR}/cmake/VerifyPackageStage.cmake")

include(CPack)
