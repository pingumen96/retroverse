# rv_platform_win: the Windows side that needs no SDL (ADR-0008 §2), GPU-free: the system probe (Win32SystemProbe,
# rv_core's ISystemProbe: registry values, key existence and sub-keys of HKCU and HKLM, files and wildcards, the home,
# System32 and Windows folders, PATH) and the Win32 process runner (Win32ProcessRunner: job object, completion port,
# watcher thread), with the Win32 helpers they share with rv_platform's window hand-over (win32_util.hpp).
# retroverse-cli and rv_tests use them without SDL.

add_library(rv_platform_win STATIC
    src/system_probe.cpp
    src/win32_process_runner.cpp
    src/win32_util.cpp
)
add_library(rv::platform_win ALIAS rv_platform_win)
rv_set_target_defaults(rv_platform_win)
target_include_directories(rv_platform_win PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# advapi32: the registry (the system probe).
target_link_libraries(rv_platform_win PUBLIC rv_core rv_launcher PRIVATE advapi32)
rv_forbid_link(rv_platform_win)
