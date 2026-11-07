# rv_launcher: emulator launch policy, GPU-free: Windows command-line quoting (the inverse of CommandLineToArgvW),
# launch requests and their validation, environment overrides, waiting strategies, exit code classification, the
# process runner port (IProcessRunner) and the launch session state machine; since I2 (ADR-0008 §8) Retroverse's own
# argument templates, emulator profiles (shipped in assets/launcher/emulators.json, the user's in
# <user data>/config/emulators.json) with their executables located through rv_core's ISystemProbe, launch plans (a
# neutral target and a profile become a request or a blocker) and the automation guard (RV_LAUNCH_ALLOWLIST); since I5
# step 8 the plan that opens a game's manual through explorer.exe (manual_open.hpp, ADR-0016 §10); since I11 step 11 the
# choice of the game's window among the tree's (window_choice.hpp, ADR-0020 §2); since I11 step 12 a launch adapted to
# its program's profile, in VR its arguments and append-file (vr_launch.hpp, ADR-0020 §1). The Win32
# process runner lives in rv_platform_win (Win32ProcessRunner, which applies the guard), the window hand-over in
# rv_platform (WindowHandover). Depends on rv_core only: the catalog's launch resolution calls into it.

add_library(rv_launcher STATIC
    src/arg_template.cpp
    src/command_line.cpp
    src/emulator_profile.cpp
    src/environment.cpp
    src/exit_status.cpp
    src/launch_guard.cpp
    src/launch_plan.cpp
    src/launch_request.cpp
    src/launch_session.cpp
    src/manual_open.cpp
    src/process_runner.cpp
    src/vr_launch.cpp
    src/window_choice.cpp
)
add_library(rv::launcher ALIAS rv_launcher)
rv_set_target_defaults(rv_launcher)
target_include_directories(rv_launcher PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rv_launcher PUBLIC rv_core)
rv_forbid_link(rv_launcher)
