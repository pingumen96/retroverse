#include "support/child_process.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/platform_win/win32_util.hpp>

namespace rv::test {
namespace {

using platform_win::lastErrorMessage;
using platform_win::UniqueHandle;

/// Owns an initialized PROC_THREAD_ATTRIBUTE_LIST with one attribute.
class AttributeList {
public:
    AttributeList() = default;
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
    ~AttributeList() {
        if (m_initialized) {
            DeleteProcThreadAttributeList(list());
        }
    }

    Status initialize() {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        m_storage.resize(size);
        if (!InitializeProcThreadAttributeList(list(), 1, 0, &size)) {
            return makeError(ErrorCode::PlatformError,
                             "InitializeProcThreadAttributeList failed: " + lastErrorMessage());
        }
        m_initialized = true;
        return ok();
    }
    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST list() noexcept {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_storage.data());
    }

private:
    std::vector<std::byte> m_storage;
    bool m_initialized = false;
};

/// An inheritable handle of a new (or truncated) file that receives an output stream.
Expected<UniqueHandle> createOutput(const std::filesystem::path& file) {
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    UniqueHandle handle(CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle) {
        return makeError(ErrorCode::IoError, "cannot create the output file: " + lastErrorMessage(),
                         core::pathToUtf8Lossy(file));
    }
    return handle;
}

} // namespace

Expected<ChildResult> runChild(const ChildRequest& request) {
    if (request.stdoutFile.empty() || request.stderrFile.empty()) {
        return makeError(ErrorCode::InvalidArgument, "name the files of the standard output and error");
    }
    auto program = core::tryPathToUtf8(request.executable);
    if (!program) {
        return propagate(std::move(program.error()));
    }
    const auto commandLineUtf8 = launcher::buildCommandLine(*program, request.arguments);
    if (!commandLineUtf8) {
        return propagate(commandLineUtf8.error());
    }
    auto commandLine = core::utf8ToWide(*commandLineUtf8);
    if (!commandLine) {
        return propagate(std::move(commandLine.error()));
    }

    std::wstring environmentBlock;
    if (!request.environment.empty()) {
        for (const launcher::EnvironmentVariable& variable :
             launcher::mergeEnvironment(platform_win::currentEnvironment(), request.environment)) {
            auto entry = core::utf8ToWide(variable.name + "=" + variable.value);
            if (!entry) {
                return makeError(ErrorCode::ValidationFailed, "an environment variable is not valid UTF-8",
                                 variable.name);
            }
            environmentBlock += *entry;
            environmentBlock.push_back(L'\0');
        }
        if (environmentBlock.empty()) {
            environmentBlock.push_back(L'\0'); // an empty block is two NULs
        }
        environmentBlock.push_back(L'\0');
    }

    const bool oneFile =
        request.stdoutFile.lexically_normal().native() == request.stderrFile.lexically_normal().native();
    auto output = createOutput(request.stdoutFile);
    if (!output) {
        return propagate(std::move(output.error()));
    }
    UniqueHandle error;
    if (!oneFile) {
        auto created = createOutput(request.stderrFile);
        if (!created) {
            return propagate(std::move(created.error()));
        }
        error = std::move(*created);
    }
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const UniqueHandle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input) {
        return makeError(ErrorCode::IoError, "cannot open NUL: " + lastErrorMessage());
    }

    // Only the standard handles are inherited, whatever else this process holds.
    std::array<HANDLE, 3> inherited{input.get(), output->get(), oneFile ? nullptr : error.get()};
    const std::size_t inheritedCount = oneFile ? 2 : 3;
    AttributeList attributes;
    if (auto initialized = attributes.initialize(); !initialized) {
        return propagate(std::move(initialized.error()));
    }
    if (!UpdateProcThreadAttribute(attributes.list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                   sizeof(HANDLE) * inheritedCount, nullptr, nullptr)) {
        return makeError(ErrorCode::PlatformError, "UpdateProcThreadAttribute failed: " + lastErrorMessage());
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = output->get();
    startup.StartupInfo.hStdError = oneFile ? output->get() : error.get();
    startup.lpAttributeList = attributes.list();

    PROCESS_INFORMATION info{};
    const auto started = std::chrono::steady_clock::now();
    if (!CreateProcessW(request.executable.c_str(), commandLine->data(), nullptr, nullptr, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        environmentBlock.empty() ? nullptr : environmentBlock.data(),
                        request.workingDirectory.empty() ? nullptr : request.workingDirectory.c_str(),
                        &startup.StartupInfo, &info)) {
        return makeError(ErrorCode::PlatformError, "cannot start the process: " + lastErrorMessage(), *program);
    }
    const UniqueHandle process(info.hProcess);
    const UniqueHandle thread(info.hThread);

    ChildResult result;
    const auto timeout = static_cast<DWORD>(std::clamp<long long>(request.timeout.count(), 0, INFINITE - 1));
    if (WaitForSingleObject(process.get(), timeout) == WAIT_TIMEOUT) {
        result.timedOut = true;
        // The test harness ends what it started (never the product: it never terminates an emulator).
        TerminateProcess(process.get(), 1);
        WaitForSingleObject(process.get(), INFINITE);
    }
    result.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.get(), &exitCode)) {
        return makeError(ErrorCode::PlatformError, "cannot read the exit code: " + lastErrorMessage(), *program);
    }
    result.exitCode = static_cast<int>(exitCode);

    output->reset();
    error.reset();
    auto standardOutput = core::readTextFile(request.stdoutFile);
    if (!standardOutput) {
        return propagate(std::move(standardOutput.error()));
    }
    result.standardOutput = std::move(*standardOutput);
    if (oneFile) {
        result.standardError = result.standardOutput;
    } else {
        auto standardError = core::readTextFile(request.stderrFile);
        if (!standardError) {
            return propagate(std::move(standardError.error()));
        }
        result.standardError = std::move(*standardError);
    }
    return result;
}

} // namespace rv::test
