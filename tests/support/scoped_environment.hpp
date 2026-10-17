#pragma once

// Environment variables of this process set for one scope and put back as they were (I2 step 12): the launch tests set
// RV_LAUNCH_ALLOWLIST to the fake emulators they start, so that the automation guard is exercised (CLAUDE.md, safety
// rules), and PATH so that the fixtures' copies of the fake emulator find their DLLs. Values are read and written with
// GetEnvironmentVariableW/SetEnvironmentVariableW, which processes started afterwards inherit and which
// launcher::processEnvironmentVariable() reads (_wgetenv would miss them). Header-only.

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rv::test {

/// The value of an environment variable of this process; nothing when it is not set (an empty value is set).
[[nodiscard]] inline std::optional<std::wstring> environmentVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return std::wstring();
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return value;
}

/// Sets an environment variable for one scope (nothing: unsets it), and puts back the value it had, or none.
class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::wstring name, const std::optional<std::wstring>& value)
        : m_name(std::move(name)), m_previous(environmentVariable(m_name.c_str())) {
        SetEnvironmentVariableW(m_name.c_str(), value ? value->c_str() : nullptr);
    }
    ~ScopedEnvironmentVariable() { SetEnvironmentVariableW(m_name.c_str(), m_previous ? m_previous->c_str() : nullptr); }
    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

    /// The value the variable had before (nothing: it was not set).
    [[nodiscard]] const std::optional<std::wstring>& previous() const noexcept { return m_previous; }

private:
    std::wstring m_name;
    std::optional<std::wstring> m_previous;
};

/// RV_LAUNCH_ALLOWLIST listing `executables` (absolute paths, ';' between them) for one scope: only they may start.
class ScopedLaunchAllowList {
public:
    explicit ScopedLaunchAllowList(std::span<const std::filesystem::path> executables)
        : m_variable(L"RV_LAUNCH_ALLOWLIST", listOf(executables)) {}

private:
    static std::wstring listOf(std::span<const std::filesystem::path> executables) {
        std::wstring list;
        for (const std::filesystem::path& executable : executables) {
            if (!list.empty()) {
                list += L';';
            }
            list += std::filesystem::absolute(executable).make_preferred().wstring();
        }
        return list;
    }

    ScopedEnvironmentVariable m_variable;
};

/// PATH with `folder` put first, for one scope.
class ScopedPathPrefix {
public:
    explicit ScopedPathPrefix(const std::filesystem::path& folder)
        : m_variable(L"PATH", std::filesystem::path(folder).make_preferred().wstring() + L";" +
                                  environmentVariable(L"PATH").value_or(std::wstring())) {}

private:
    ScopedEnvironmentVariable m_variable;
};

} // namespace rv::test
