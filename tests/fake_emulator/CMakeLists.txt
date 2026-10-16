# rv_fake_emulator: the stand-in emulator of the launch tests (exit codes, crashes, hangs, a wrapper that starts a copy
# of itself, fullscreen and exclusive windows, round trips of arguments, environment and standard streams, an exit
# timeout so that an abandoned copy ends by itself). Started as a process from the shared runtime output directory by
# rv_tests (the Win32 process runner and the child runner of tests/support, in its windowless modes) and by the launch
# spike's desktop cases (spikes/launch). Built whenever the tests or the spikes are; it links SDL3 for its window modes,
# which never makes a test executable link SDL: they only start it.

add_executable(rv_fake_emulator fake_emulator.cpp)
rv_set_target_defaults(rv_fake_emulator)
# It receives library-like paths in the launch tests of I2 (ADR-0008 §1).
rv_enable_long_paths(rv_fake_emulator)
target_link_libraries(rv_fake_emulator PRIVATE rv_core rv_launcher SDL3::SDL3)
