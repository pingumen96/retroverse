# rv_platform: SDL3 library lifetime, window and OpenGL context creation, event pump, dialogs, and input: SDL events
# to rv::input frames, gamepad hot-plug, mouse capture. The only first-party library (besides rv_app and rv_devui)
# allowed to include SDL. Also the Windows side of emulator launches (rv_launcher's port): the Win32 process runner
# (job object, completion port, watcher thread) and the hand-over of the window to the emulator and back.

add_library(rv_platform STATIC
    src/dialogs.cpp
    src/event_pump.cpp
    src/gamepad_manager.cpp
    src/graphics_window.cpp
    src/mouse_capture.cpp
    src/sdl_input_codes.cpp
    src/sdl_input_collector.cpp
    src/sdl_platform.cpp
    src/win32_process_runner.cpp
    src/win32_util.cpp
    src/window_handover.cpp
)
add_library(rv::platform ALIAS rv_platform)
rv_set_target_defaults(rv_platform)
target_include_directories(rv_platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_platform PUBLIC rv_core rv_input rv_launcher SDL3::SDL3)
