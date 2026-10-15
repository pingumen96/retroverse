# rv_test_support: helpers shared by the test executables. GPU-free.
# Sources are globbed so independent work streams can add helpers without editing this file.

file(GLOB RV_TEST_SUPPORT_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
add_library(rv_test_support STATIC ${RV_TEST_SUPPORT_SOURCES})
rv_set_target_defaults(rv_test_support)
# Tests include helpers as "support/<file>.hpp".
target_include_directories(rv_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(rv_test_support PUBLIC rv_core)
rv_forbid_link(rv_test_support)
