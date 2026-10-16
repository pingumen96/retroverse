#include <rv/platform_win/win32_util.hpp>

#include <rv/core/system_error.hpp>

namespace rv::platform_win {

std::string errorMessage(DWORD error) {
    return core::systemErrorMessage(error);
}

std::string lastErrorMessage() {
    return core::systemErrorMessage(GetLastError());
}

} // namespace rv::platform_win
