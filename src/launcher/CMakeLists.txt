# rv_launcher: emulator launch policy, GPU-free: Windows command-line quoting, launch requests and waiting strategies,
# the launch session state machine and the classification of its outcome. The Win32 process runner and the window
# hand-over live in rv_platform.

add_library(rv_launcher STATIC
    src/launcher_module.cpp
)
add_library(rv::launcher ALIAS rv_launcher)
rv_set_target_defaults(rv_launcher)
target_include_directories(rv_launcher PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_launcher PUBLIC rv_core)
rv_forbid_link(rv_launcher)
