# rv_platform: SDL3 window, OpenGL context creation, event pump, gamepad hot-plug and the SDL -> rv_input
# event collector. The only first-party library (besides rv_app and rv_devui) allowed to include SDL.

add_library(rv_platform STATIC
    src/platform_module.cpp
)
add_library(rv::platform ALIAS rv_platform)
rv_set_target_defaults(rv_platform)
target_include_directories(rv_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_platform PUBLIC rv_core rv_input SDL3::SDL3)
