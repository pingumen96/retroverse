#pragma once

// Win32 helpers shared by rv_platform_win and rv_platform (the window hand-over): an owning HANDLE, FILETIME ticks and
// error messages. Windows-only. Text conversions are rv::core's (rv/core/utf8.hpp).

#include <cstdint>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rv::platform_win {

/// "<system message> (error <code>)" for a Win32 error code (rv::core::systemErrorMessage).
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

/// "0x80070002" for an HRESULT.
[[nodiscard]] std::string hresultText(long result);

/// COM on the calling thread for a scope (I11 step 13): CoInitializeEx with `apartment` (COINIT_MULTITHREADED or
/// COINIT_APARTMENTTHREADED) and COINIT_DISABLE_OLE1DDE, undone on destruction when this call did it. A thread already
/// in the other kind of apartment keeps it, which IShellLinkW and ShellExecuteExW work in too.
class ComScope {
public:
    explicit ComScope(unsigned long apartment) noexcept;
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope();

    /// COM can be used on this thread now.
    [[nodiscard]] bool usable() const noexcept;
    [[nodiscard]] long result() const noexcept { return m_result; }

private:
    long m_result = 0;
};

} // namespace rv::platform_win
