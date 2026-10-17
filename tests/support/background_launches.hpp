#pragma once

// Automated tests never take the Windows foreground (I2 step focus, docs/i2/step-focus.md): the test executables that
// start programs (rv_tests, rv_smoke_tests) set RV_LAUNCH_BACKGROUND=1 in their own environment before any test runs,
// unless it is set already (the desktop tests' registrations set it to "0"; the unit and gpu test presets to "1"). Every
// program they start inherits it: Win32ProcessRunner, in the tests and in the retroverse.exe and retroverse-cli
// processes they run, then starts console programs without a console window, GUI programs minimized and inactive, and
// grants no foreground; rv_fake_emulator keeps its windows off-screen and inactive and injects no input. Header-only.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rv::test {

/// Sets RV_LAUNCH_BACKGROUND=1 in this process's environment (SetEnvironmentVariableW, which programs started later
/// inherit and the runner reads) unless the variable is set, to any value. Returns whether it is now asked for.
inline bool ensureBackgroundLaunches() noexcept {
    wchar_t value[8] = {};
    SetLastError(ERROR_SUCCESS);
    const DWORD length = GetEnvironmentVariableW(L"RV_LAUNCH_BACKGROUND", value, 8);
    if (length == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        return SetEnvironmentVariableW(L"RV_LAUNCH_BACKGROUND", L"1") != FALSE;
    }
    return length != 0 && !(length == 1 && value[0] == L'0');
}

} // namespace rv::test
