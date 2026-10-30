# rv_platform_win: the Windows side that needs no SDL (ADR-0008 §2), GPU-free: the system probe (Win32SystemProbe,
# rv_core's ISystemProbe: registry values, key existence and sub-keys of HKCU and HKLM, files and wildcards, the home,
# System32 and Windows folders, PATH) and the Win32 process runner (Win32ProcessRunner: job object, completion port,
# watcher thread) and the process's priority class (I5 step 2, ADR-0016 §2), with the Win32 helpers they share with
# rv_platform's window hand-over (win32_util.hpp). retroverse-cli and rv_tests use them without SDL.

add_library(rv_platform_win STATIC
    src/process_priority.cpp
    src/system_probe.cpp
    src/win32_process_runner.cpp
    src/win32_util.cpp
)
add_library(rv::platform_win ALIAS rv_platform_win)
rv_set_target_defaults(rv_platform_win)
target_include_directories(rv_platform_win PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# advapi32: the registry (the system probe); ntdll: the system's process list (the liveness of a process this user may
# not open).
target_link_libraries(rv_platform_win PUBLIC rv_core rv_launcher PRIVATE advapi32 ntdll)
rv_forbid_link(rv_platform_win)

# rv_platform_win_capture (I10 step 6, ADR-0017 §7): a window's picture through Windows.Graphics.Capture (C++/WinRT, the
# Windows SDK's headers) into a D3D11 texture, for the VR mode's game on the CRT. Apart from rv_platform_win, so that
# retroverse-cli and rv_tests stay without D3D11 and WinRT; rv_app alone links it.
add_library(rv_platform_win_capture STATIC
    src/window_capture.cpp
)
add_library(rv::platform_win_capture ALIAS rv_platform_win_capture)
rv_set_target_defaults(rv_platform_win_capture)
target_include_directories(rv_platform_win_capture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_platform_win_capture PUBLIC rv_core glm::glm PRIVATE d3d11 dxgi windowsapp)
