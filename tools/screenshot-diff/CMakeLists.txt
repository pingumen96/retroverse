# retroverse-screenshot-diff (style guide §26.1; the Gate A tolerance in docs/testing/gate-a.md): compares a screenshot
# with its golden image within a tolerance (per-channel threshold, share of differing pixels) and writes a heatmap of
# the differences. Built in every configuration: it needs neither a window nor OpenGL.

# GPU-free core, shared by the tool and the GPU smoke tests (tests/smoke: the golden comparison of G01) and unit-tested
# by tests/unit/screenshot_diff.
add_library(rv_screenshot_diff_core STATIC
    src/screenshot_diff.cpp
)
add_library(rv::screenshot_diff_core ALIAS rv_screenshot_diff_core)
rv_set_target_defaults(rv_screenshot_diff_core)
target_include_directories(rv_screenshot_diff_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_screenshot_diff_core PUBLIC rv_core rv_assets)
rv_forbid_link(rv_screenshot_diff_core)

add_executable(retroverse-screenshot-diff src/main.cpp)
rv_set_target_defaults(retroverse-screenshot-diff)
target_link_libraries(retroverse-screenshot-diff PRIVATE rv_screenshot_diff_core)
