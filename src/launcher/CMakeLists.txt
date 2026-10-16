# rv_launcher: emulator launch policy, GPU-free: Windows command-line quoting (the inverse of CommandLineToArgvW),
# launch requests and their validation, environment overrides, waiting strategies, exit code classification, the
# process runner port (IProcessRunner) and the launch session state machine. The Win32 process runner lives in
# rv_platform_win (Win32ProcessRunner), the window hand-over in rv_platform (WindowHandover).

add_library(rv_launcher STATIC
    src/command_line.cpp
    src/environment.cpp
    src/exit_status.cpp
    src/launch_request.cpp
    src/launch_session.cpp
    src/process_runner.cpp
)
add_library(rv::launcher ALIAS rv_launcher)
rv_set_target_defaults(rv_launcher)
target_include_directories(rv_launcher PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_launcher PUBLIC rv_core)
rv_forbid_link(rv_launcher)
