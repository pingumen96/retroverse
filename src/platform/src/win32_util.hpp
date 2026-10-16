#pragma once

// Private Win32 helpers of rv_platform: UTF-8 <-> UTF-16, error messages, an owning HANDLE. Windows-only.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rv/core/expected.hpp>

namespace rv::platform::win32 {

/// UTF-8 to UTF-16; malformed UTF-8 is an error (never silently replaced).
[[nodiscard]] Expected<std::wstring> widen(std::string_view utf8);
/// UTF-16 to UTF-8; unpaired surrogates become U+FFFD (for messages and names).
[[nodiscard]] std::string narrow(std::wstring_view utf16);
/// "<system message> (error <code>)" for a Win32 error code.
[[nodiscard]] std::string errorMessage(DWORD error);
/// errorMessage(GetLastError()).
[[nodiscard]] std::string lastErrorMessage();

/// Owns a kernel HANDLE (CloseHandle on destruction). Null and INVALID_HANDLE_VALUE both mean "none".
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : m_handle(handle) {}
    UniqueHandle(UniqueHandle&& other) noexcept : m_handle(std::exchange(other.m_handle, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.m_handle, nullptr));
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    ~UniqueHandle() { reset(); }

    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return valid(); }
    /// Gives up ownership.
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(m_handle, nullptr); }
    void reset(HANDLE handle = nullptr) noexcept {
        if (valid()) {
            CloseHandle(m_handle);
        }
        m_handle = handle;
    }

private:
    HANDLE m_handle = nullptr;
};

/// 100-nanosecond ticks of a FILETIME.
[[nodiscard]] inline std::uint64_t ticks(const FILETIME& time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

} // namespace rv::platform::win32
