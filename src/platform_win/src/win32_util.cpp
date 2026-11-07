#include <rv/platform_win/win32_util.hpp>

#include <objbase.h>

#include <fmt/format.h>

#include <rv/core/system_error.hpp>

namespace rv::platform_win {

std::string errorMessage(DWORD error) {
    return core::systemErrorMessage(error);
}

std::string lastErrorMessage() {
    return core::systemErrorMessage(GetLastError());
}

std::string hresultText(long result) {
    return fmt::format("0x{:08X}", static_cast<unsigned long>(result));
}

ComScope::ComScope(unsigned long apartment) noexcept
    : m_result(CoInitializeEx(nullptr, static_cast<DWORD>(apartment) | COINIT_DISABLE_OLE1DDE)) {}

ComScope::~ComScope() {
    if (SUCCEEDED(m_result)) {
        CoUninitialize(); // S_OK and S_FALSE alike: each successful call is undone once
    }
}

bool ComScope::usable() const noexcept {
    return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
}

} // namespace rv::platform_win
