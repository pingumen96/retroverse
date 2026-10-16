# rv_platform: SDL3 library lifetime, window and OpenGL context creation, event pump, dialogs, and input: SDL events
# to rv::input frames, gamepad hot-plug, mouse capture. The only first-party library (besides rv_app and rv_devui)
# allowed to include SDL. Also the hand-over of the window to an emulator and back (WindowHandover); the Win32 process
# runner it pairs with lives in rv_platform_win, which needs no SDL and which rv_platform links for its users.

add_library(rv_platform STATIC
    src/dialogs.cpp
    src/event_pump.cpp
    src/gamepad_manager.cpp
    src/graphics_window.cpp
    src/mouse_capture.cpp
    src/sdl_input_codes.cpp
    src/sdl_input_collector.cpp
    src/sdl_platform.cpp
    src/window_handover.cpp
)
add_library(rv::platform ALIAS rv_platform)
rv_set_target_defaults(rv_platform)
target_include_directories(rv_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_platform PUBLIC rv_core rv_input rv_launcher rv_platform_win SDL3::SDL3)
