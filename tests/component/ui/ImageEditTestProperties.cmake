# Applied after PRE_TEST discovery. Cold SAM2 session loading plus inference took
# 17.8 s with the local two-CPU policy; this is a functional integration test, not
# a latency benchmark. Keep all other image-edit tests at their existing 10 s.
set(dvs_model_case "ui.ImageEditControllerTests.SmartSegmentWithStagedModelExecutesAndCanUndo")
set(dvs_model_matches ${dvs_image_edit_controller_tests_TESTS})
list(FILTER dvs_model_matches INCLUDE REGEX "^${dvs_model_case}$")
list(LENGTH dvs_model_matches dvs_model_match_count)
if(NOT dvs_model_match_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one discovered SAM2 integration test; found ${dvs_model_match_count}.")
endif()
set_tests_properties("${dvs_model_case}" PROPERTIES TIMEOUT 60)
