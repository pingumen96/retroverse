# rv_platform_win: the Windows side that needs no SDL (ADR-0008 §2), GPU-free: the system probe (rv_core's
# ISystemProbe: registry values and App Paths in HKCU then HKLM, the uninstall key, the executable search, file and
# wildcard checks) and, from I2 step 0b, the Win32 process runner moved out of rv_platform, so that retroverse-cli and
# rv_tests use them without SDL. Skeleton: each file names the I2 step that fills it.

add_library(rv_platform_win STATIC
    src/system_probe.cpp
)
add_library(rv::platform_win ALIAS rv_platform_win)
rv_set_target_defaults(rv_platform_win)
target_include_directories(rv_platform_win PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
# advapi32: the registry (the system probe).
target_link_libraries(rv_platform_win PUBLIC rv_core rv_launcher PRIVATE advapi32)
rv_forbid_link(rv_platform_win)
