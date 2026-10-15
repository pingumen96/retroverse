#include "child_process.hpp"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string_view>

#include <windows.h>

#include <rv/core/text.hpp>

namespace rv::smoke {
namespace {

/// Closes a Win32 handle on scope exit.
class HandleGuard {
public:
    explicit HandleGuard(HANDLE handle) noexcept : m_handle(handle) {}
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    ~HandleGuard() {
        if (valid()) {
            CloseHandle(m_handle);
        }
    }
    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_handle;
};

std::wstring widen(std::string_view utf8) {
    return core::pathFromUtf8(utf8).wstring();
}

bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towlower(static_cast<wint_t>(x)) == std::towlower(static_cast<wint_t>(y));
           });
}

/// The environment block of this process (NUL-separated "name=value" entries, a final NUL) without `removed`.
std::wstring childEnvironment(const std::vector<std::string>& removed) {
    std::vector<std::wstring> removedNames;
    for (const std::string& name : removed) {
        removedNames.push_back(widen(name));
    }
    std::wstring block;
    wchar_t* strings = GetEnvironmentStringsW();
    if (strings == nullptr) {
        return std::wstring(2, L'\0');
    }
    for (const wchar_t* entry = strings; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        const std::wstring_view text(entry);
        // Entries such as "=C:=C:\dir" start with '=': their name ends at the second one.
        const std::wstring_view name = text.substr(0, text.find(L'=', 1));
        const bool drop = std::any_of(removedNames.begin(), removedNames.end(), [&](const std::wstring& removedName) {
            return equalsIgnoreCase(name, removedName);
        });
        if (!drop) {
            block.append(text);
            block.push_back(L'\0');
        }
    }
    FreeEnvironmentStringsW(strings);
    if (block.empty()) {
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::string lastErrorText() {
    return "Windows error " + std::to_string(GetLastError());
}

} // namespace

std::wstring quoteArgument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring quoted = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            // Backslashes before the closing quote are doubled, so it stays a quote.
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(*it);
        }
    }
    quoted.push_back(L'"');
    return quoted;
}

Expected<ProcessOutcome> runProcess(const ProcessRequest& request) {
    std::wstring commandLine = quoteArgument(request.executable.wstring());
    for (const std::string& argument : request.arguments) {
        commandLine += L' ';
        commandLine += quoteArgument(widen(argument));
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    const HandleGuard output(CreateFileW(request.outputFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!output.valid()) {
        return makeError(ErrorCode::IoError, "cannot create the output file: " + lastErrorText(),
                         core::pathToUtf8(request.outputFile));
    }
    const HandleGuard input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input.valid()) {
        return makeError(ErrorCode::IoError, "cannot open NUL: " + lastErrorText());
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.get();
    startup.hStdOutput = output.get();
    startup.hStdError = output.get();
    std::wstring environment = childEnvironment(request.removedVariables);
    PROCESS_INFORMATION process{};
    const auto started = std::chrono::steady_clock::now();
    if (!CreateProcessW(request.executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment.data(),
                        request.workingDirectory.c_str(), &startup, &process)) {
        return makeError(ErrorCode::PlatformError, "cannot start the process: " + lastErrorText(),
                         core::pathToUtf8(request.executable));
    }
    const HandleGuard processHandle(process.hProcess);
    const HandleGuard threadHandle(process.hThread);

    ProcessOutcome outcome;
    const auto timeout = static_cast<DWORD>(std::clamp<long long>(request.timeout.count(), 0, INFINITE - 1));
    if (WaitForSingleObject(processHandle.get(), timeout) == WAIT_TIMEOUT) {
        outcome.timedOut = true;
        TerminateProcess(processHandle.get(), 1);
        WaitForSingleObject(processHandle.get(), INFINITE);
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
        return makeError(ErrorCode::PlatformError, "cannot read the exit code: " + lastErrorText(),
                         core::pathToUtf8(request.executable));
    }
    outcome.exitCode = static_cast<int>(exitCode);
    outcome.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    return outcome;
}

} // namespace rv::smoke
