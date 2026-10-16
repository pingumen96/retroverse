#include <rv/platform/win32_process_runner.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/text.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/launcher/launch_request.hpp>

#include "win32_util.hpp"

namespace rv::platform {
namespace {

using launcher::LaunchClock;
using launcher::ProcessEvent;
using launcher::ProcessEventKind;
using win32::UniqueHandle;

/// Completion keys of the job's port: the job's notifications, and the packet that stops the watcher.
constexpr ULONG_PTR kJobKey = 1;
constexpr ULONG_PTR kStopKey = 2;
/// Processes of a tree whose exit codes and image names are kept (the rest are still counted by the job).
constexpr std::size_t kMaxTrackedProcesses = 256;
/// Processes of a tree whose windows are looked for.
constexpr std::size_t kMaxWindowProcesses = 64;
/// How long an exiting process may take to be signaled after the job reported its exit.
constexpr DWORD kExitSettleMilliseconds = 2000;

/// LaunchClock time of a past system time (UTC FILETIME): now, minus how long ago it was.
LaunchClock::time_point toLaunchClock(const FILETIME& systemTime) noexcept {
    const LaunchClock::time_point now = LaunchClock::now();
    FILETIME current{};
    GetSystemTimePreciseAsFileTime(&current);
    const std::uint64_t then = win32::ticks(systemTime);
    const std::uint64_t nowTicks = win32::ticks(current);
    if (then == 0 || then > nowTicks) {
        return now;
    }
    const auto ago = std::chrono::nanoseconds(static_cast<std::int64_t>((nowTicks - then) * 100u));
    return now - std::chrono::duration_cast<LaunchClock::duration>(ago);
}

/// When an exited process ended, by the system's account; `fallback` when it cannot be read. Never after `fallback`
/// (the moment the exit was noticed).
LaunchClock::time_point exitTimeOf(HANDLE process, LaunchClock::time_point fallback) noexcept {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return fallback;
    }
    return std::min(toLaunchClock(exit), fallback);
}

/// File name of a process's executable, or empty.
std::string imageName(HANDLE process) {
    std::array<wchar_t, 1024> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        return {};
    }
    const std::wstring_view path(buffer.data(), size);
    const std::size_t slash = path.find_last_of(L"\\/");
    return win32::narrow(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

struct WindowSearch {
    const DWORD* processIds = nullptr;
    std::size_t count = 0;
    bool found = false;
};

BOOL CALLBACK findVisibleWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    if (!IsWindowVisible(window)) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (std::find(search->processIds, search->processIds + search->count, processId) ==
        search->processIds + search->count) {
        return TRUE;
    }
    RECT rect{};
    if (!GetWindowRect(window, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
        return TRUE;
    }
    search->found = true;
    return FALSE;
}

/// A started process followed by a watcher thread. Never terminates it (once it has run).
class Win32RunningProcess final : public launcher::IRunningProcess {
public:
    Win32RunningProcess(UniqueHandle process, std::uint32_t processId, UniqueHandle job, UniqueHandle port,
                        std::function<void()> wake, std::chrono::milliseconds pollInterval)
        : m_process(std::move(process)), m_processId(processId), m_job(std::move(job)), m_port(std::move(port)),
          m_stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)), m_wake(std::move(wake)),
          m_pollMilliseconds(static_cast<DWORD>(std::clamp<std::int64_t>(pollInterval.count(), 1, 10000))) {}

    Win32RunningProcess(const Win32RunningProcess&) = delete;
    Win32RunningProcess& operator=(const Win32RunningProcess&) = delete;

    ~Win32RunningProcess() override {
        m_stopping.store(true);
        if (m_stopEvent) {
            SetEvent(m_stopEvent.get());
        }
        if (m_port) {
            PostQueuedCompletionStatus(m_port.get(), 0, kStopKey, nullptr);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        // The handles close now. The job has no JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: its processes keep running.
    }

    [[nodiscard]] std::uint32_t processId() const noexcept override { return m_processId; }
    [[nodiscard]] bool followsTree() const noexcept override { return m_job.valid(); }
    [[nodiscard]] HANDLE processHandle() const noexcept { return m_process.get(); }

    void takeEvents(std::vector<ProcessEvent>& out) override {
        const std::lock_guard lock(m_mutex);
        if (m_queue.empty()) {
            return;
        }
        for (ProcessEvent& event : m_queue) {
            out.push_back(std::move(event));
        }
        m_queue.clear();
    }

    /// Starts the watcher thread (before the process is resumed).
    [[nodiscard]] Status startWatching() {
        if (!m_stopEvent) {
            return makeError(ErrorCode::PlatformError, "CreateEventW failed: " + win32::lastErrorMessage());
        }
        try {
            m_queue.reserve(16);
            m_thread = std::thread([this] { watch(); });
        } catch (const std::exception& e) {
            return makeError(ErrorCode::PlatformError, std::string("the watcher thread could not start: ") + e.what());
        }
        return ok();
    }

private:
    struct Tracked {
        std::uint32_t processId = 0;
        UniqueHandle handle;
        std::string image;
    };

    void watch() noexcept {
        try {
            if (!waitForMainExit()) {
                return;
            }
            if (!m_job) {
                checkWindows(); // the started process's windows are gone with it
                return;
            }
            watchTree();
        } catch (const std::exception& e) {
            reportFailure(std::string("unexpected error: ") + e.what());
        } catch (...) {
            reportFailure("unexpected error");
        }
    }

    /// Blocks until the started process exits (true) or watching stops (false). Between the job's notifications and
    /// the window checks, nothing runs.
    bool waitForMainExit() {
        const std::array<HANDLE, 2> handles{m_process.get(), m_stopEvent.get()};
        while (true) {
            const DWORD result =
                WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, m_pollMilliseconds);
            if (result == WAIT_OBJECT_0) {
                // Processes it started before exiting are reported first: the events follow the order of causes.
                if (m_port) {
                    drainJobMessages(0);
                }
                reportMainExit();
                return true;
            }
            if (result == WAIT_OBJECT_0 + 1 || m_stopping.load()) {
                return false;
            }
            if (result != WAIT_TIMEOUT) {
                reportFailure("waiting for the process failed: " + win32::lastErrorMessage());
                return false;
            }
            if (m_port) {
                drainJobMessages(0);
                if (m_stopping.load()) {
                    return false;
                }
            }
            checkWindows();
        }
    }

    /// After the started process: the rest of the tree, until it is empty or watching stops.
    void watchTree() {
        if (m_activeZeroPending) {
            reportTreeEmpty();
            return;
        }
        auto nextWindowCheck = LaunchClock::now();
        while (!m_treeEmptyReported && !m_stopping.load()) {
            const bool received = drainJobMessages(m_pollMilliseconds);
            if (m_treeEmptyReported || m_stopping.load()) {
                break;
            }
            if (!received) {
                // Job notifications are not guaranteed: count the processes left.
                JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
                if (QueryInformationJobObject(m_job.get(), JobObjectBasicAccountingInformation, &accounting,
                                              sizeof(accounting), nullptr) &&
                    accounting.ActiveProcesses == 0) {
                    reportTreeEmpty();
                    break;
                }
            }
            if (const auto now = LaunchClock::now(); now >= nextWindowCheck) {
                checkWindows();
                nextWindowCheck = now + std::chrono::milliseconds(m_pollMilliseconds);
            }
        }
    }

    /// Handles the job's queued notifications, waiting up to `timeoutMilliseconds` for the first. True when at least
    /// one notification arrived.
    bool drainJobMessages(DWORD timeoutMilliseconds) {
        bool received = false;
        DWORD timeout = timeoutMilliseconds;
        while (true) {
            DWORD message = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED overlapped = nullptr;
            if (!GetQueuedCompletionStatus(m_port.get(), &message, &key, &overlapped, timeout) &&
                overlapped == nullptr) {
                const DWORD error = GetLastError();
                if (error != WAIT_TIMEOUT) {
                    reportFailure("the job's notifications failed: " + win32::errorMessage(error));
                    m_stopping.store(true);
                }
                return received;
            }
            timeout = 0;
            if (key == kStopKey) {
                m_stopping.store(true);
                return received;
            }
            if (key != kJobKey) {
                continue;
            }
            received = true;
            const auto processId = static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(overlapped));
            handleJobMessage(message, processId);
            if (m_treeEmptyReported) {
                return received;
            }
        }
    }

    void handleJobMessage(DWORD message, std::uint32_t processId) {
        switch (message) {
        case JOB_OBJECT_MSG_NEW_PROCESS:
            if (processId != m_processId) {
                trackChild(processId);
            }
            break;
        case JOB_OBJECT_MSG_EXIT_PROCESS:
        case JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS:
            if (processId != m_processId) {
                reportChildExit(processId);
            }
            break;
        case JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO:
            if (m_mainExited) {
                reportTreeEmpty();
            } else {
                m_activeZeroPending = true; // the started process's exit is reported first
            }
            break;
        default:
            break;
        }
    }

    void trackChild(std::uint32_t processId) {
        const LaunchClock::time_point now = LaunchClock::now();
        ProcessEvent event;
        event.kind = ProcessEventKind::ChildStarted;
        event.observedAt = now;
        event.occurredAt = now;
        event.processId = processId;
        if (m_tracked.size() < kMaxTrackedProcesses) {
            UniqueHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId));
            BOOL inJob = FALSE;
            if (handle && (!IsProcessInJob(handle.get(), m_job.get(), &inJob) || !inJob)) {
                handle.reset(); // the id was reused by a process outside the tree
            }
            if (handle) {
                event.image = imageName(handle.get());
            } else {
                m_untrackedExit = true;
            }
            m_tracked.push_back(Tracked{processId, std::move(handle), event.image});
        } else {
            m_untrackedExit = true;
        }
        push(std::move(event));
    }

    void reportChildExit(std::uint32_t processId) {
        const LaunchClock::time_point now = LaunchClock::now();
        ProcessEvent event;
        event.kind = ProcessEventKind::ChildExited;
        event.observedAt = now;
        event.occurredAt = now;
        event.processId = processId;
        const auto tracked = std::find_if(m_tracked.begin(), m_tracked.end(),
                                          [&](const Tracked& entry) { return entry.processId == processId; });
        if (tracked != m_tracked.end() && tracked->handle) {
            event.image = tracked->image;
            DWORD code = 0;
            // The job reports the exit while the process is being torn down, a moment before its handle is
            // signaled and its exit code final.
            if (WaitForSingleObject(tracked->handle.get(), kExitSettleMilliseconds) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(tracked->handle.get(), &code)) {
                event.exitCode = code;
            }
            event.occurredAt = exitTimeOf(tracked->handle.get(), now);
            tracked->handle.reset();
        } else {
            m_untrackedExit = true;
        }
        m_lastExitTime = std::max(m_lastExitTime, event.occurredAt);
        push(std::move(event));
    }

    void reportMainExit() {
        const LaunchClock::time_point now = LaunchClock::now();
        ProcessEvent event;
        event.kind = ProcessEventKind::MainExited;
        event.observedAt = now;
        event.processId = m_processId;
        DWORD code = 0;
        if (GetExitCodeProcess(m_process.get(), &code)) {
            event.exitCode = code;
        }
        event.occurredAt = exitTimeOf(m_process.get(), now);
        m_mainExited = true;
        m_lastExitTime = std::max(m_lastExitTime, event.occurredAt);
        push(std::move(event));
    }

    void reportTreeEmpty() {
        m_treeEmptyReported = true;
        const LaunchClock::time_point now = LaunchClock::now();
        ProcessEvent event;
        event.kind = ProcessEventKind::TreeEmpty;
        event.observedAt = now;
        // The last exit time is known when every process of the tree was tracked; otherwise only the moment it was
        // noticed bounds it.
        event.occurredAt = m_untrackedExit ? now : std::min(m_lastExitTime, now);
        push(std::move(event));
    }

    /// Reports WindowShown / WindowsGone when the tree's visibility changed since the last check.
    void checkWindows() {
        std::array<DWORD, kMaxWindowProcesses> processIds{};
        std::size_t count = 0;
        if (m_job) {
            constexpr std::size_t kListBytes =
                sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + (kMaxWindowProcesses - 1) * sizeof(ULONG_PTR);
            alignas(JOBOBJECT_BASIC_PROCESS_ID_LIST) std::array<std::byte, kListBytes> storage{};
            auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
            // A list longer than the buffer fails with ERROR_MORE_DATA after filling it: the first ones count.
            QueryInformationJobObject(m_job.get(), JobObjectBasicProcessIdList, list, static_cast<DWORD>(kListBytes),
                                      nullptr);
            const std::size_t listed = std::min<std::size_t>(list->NumberOfProcessIdsInList, kMaxWindowProcesses);
            for (std::size_t i = 0; i < listed; ++i) {
                processIds[count++] = static_cast<DWORD>(list->ProcessIdList[i]);
            }
        } else if (!m_mainExited) {
            processIds[count++] = m_processId;
        }
        WindowSearch search{processIds.data(), count, false};
        if (count > 0) {
            EnumWindows(&findVisibleWindow, reinterpret_cast<LPARAM>(&search));
        }
        if (search.found == m_windowVisible) {
            return;
        }
        m_windowVisible = search.found;
        const LaunchClock::time_point now = LaunchClock::now();
        ProcessEvent event;
        event.kind = search.found ? ProcessEventKind::WindowShown : ProcessEventKind::WindowsGone;
        event.observedAt = now;
        event.occurredAt = now;
        push(std::move(event));
    }

    void reportFailure(std::string message) noexcept {
        try {
            ProcessEvent event;
            event.kind = ProcessEventKind::WatchFailed;
            event.observedAt = LaunchClock::now();
            event.occurredAt = event.observedAt;
            event.message = std::move(message);
            push(std::move(event));
        } catch (...) {
            // Nothing left to report with.
        }
    }

    void push(ProcessEvent event) {
        {
            const std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(event));
        }
        if (m_wake) {
            m_wake();
        }
    }

    UniqueHandle m_process;
    std::uint32_t m_processId = 0;
    UniqueHandle m_job;
    UniqueHandle m_port;
    UniqueHandle m_stopEvent;
    std::function<void()> m_wake;
    DWORD m_pollMilliseconds = 100;
    std::atomic<bool> m_stopping{false};
    std::mutex m_mutex;
    std::vector<ProcessEvent> m_queue;
    std::thread m_thread;

    // Watcher thread only.
    std::vector<Tracked> m_tracked;
    LaunchClock::time_point m_lastExitTime{};
    bool m_mainExited = false;
    bool m_activeZeroPending = false;
    bool m_treeEmptyReported = false;
    bool m_untrackedExit = false;
    bool m_windowVisible = false;
};

/// A job object whose completion port reports its processes; nothing (and `error` set) when it cannot be set up.
void createJob(UniqueHandle& job, UniqueHandle& port, std::string& error) {
    UniqueHandle newJob(CreateJobObjectW(nullptr, nullptr));
    if (!newJob) {
        error = "CreateJobObjectW failed: " + win32::lastErrorMessage();
        return;
    }
    // No JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: closing the handle (Retroverse exiting) leaves the processes running.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!SetInformationJobObject(newJob.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = "SetInformationJobObject (limits) failed: " + win32::lastErrorMessage();
        return;
    }
    UniqueHandle newPort(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!newPort) {
        error = "CreateIoCompletionPort failed: " + win32::lastErrorMessage();
        return;
    }
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey = reinterpret_cast<PVOID>(kJobKey);
    association.CompletionPort = newPort.get();
    if (!SetInformationJobObject(newJob.get(), JobObjectAssociateCompletionPortInformation, &association,
                                 sizeof(association))) {
        error = "SetInformationJobObject (completion port) failed: " + win32::lastErrorMessage();
        return;
    }
    job = std::move(newJob);
    port = std::move(newPort);
}

/// The standard output and error of a captured launch (appended to `path`) and its standard input (NUL), both
/// inheritable.
Status openOutputLog(const std::filesystem::path& path, std::string_view commandLine, UniqueHandle& output,
                     UniqueHandle& input) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    UniqueHandle file(CreateFileW(path.c_str(), FILE_APPEND_DATA | SYNCHRONIZE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inheritable, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        return makeError(ErrorCode::IoError, "the output log cannot be opened: " + win32::lastErrorMessage(),
                         core::pathToUtf8(path));
    }
    SYSTEMTIME local{};
    GetLocalTime(&local);
    const std::string header =
        fmt::format("--- {:04}-{:02}-{:02} {:02}:{:02}:{:02} launch: {}\r\n", local.wYear, local.wMonth, local.wDay,
                    local.wHour, local.wMinute, local.wSecond, commandLine);
    DWORD written = 0;
    WriteFile(file.get(), header.data(), static_cast<DWORD>(header.size()), &written, nullptr);
    UniqueHandle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                                 0, nullptr));
    if (!nul) {
        return makeError(ErrorCode::IoError, "the NUL device cannot be opened: " + win32::lastErrorMessage());
    }
    output = std::move(file);
    input = std::move(nul);
    return ok();
}

/// Owns an initialized PROC_THREAD_ATTRIBUTE_LIST.
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

    [[nodiscard]] Status initialize(DWORD attributeCount) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, attributeCount, 0, &size);
        if (size == 0) {
            return makeError(ErrorCode::PlatformError,
                             "InitializeProcThreadAttributeList failed: " + win32::lastErrorMessage());
        }
        m_storage.resize(size);
        if (!InitializeProcThreadAttributeList(list(), attributeCount, 0, &size)) {
            return makeError(ErrorCode::PlatformError,
                             "InitializeProcThreadAttributeList failed: " + win32::lastErrorMessage());
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

} // namespace

std::vector<launcher::EnvironmentVariable> currentEnvironment() {
    std::vector<launcher::EnvironmentVariable> variables;
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr) {
        return variables;
    }
    for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
        const std::wstring_view text(entry);
        // A leading '=' belongs to the name of the per-drive variables ("=C:=C:\\Games").
        const std::size_t equals = text.find(L'=', 1);
        if (equals == std::wstring_view::npos) {
            continue;
        }
        variables.push_back(launcher::EnvironmentVariable{win32::narrow(text.substr(0, equals)),
                                                          win32::narrow(text.substr(equals + 1))});
    }
    FreeEnvironmentStringsW(block);
    return variables;
}

Expected<std::unique_ptr<launcher::IRunningProcess>> Win32ProcessRunner::start(const launcher::LaunchRequest& request,
                                                                               launcher::StartCallbacks callbacks) {
    const auto begin = std::chrono::steady_clock::now();
    const auto commandLineUtf8 = launcher::commandLineOf(request);
    if (!commandLineUtf8) {
        return propagate(commandLineUtf8.error());
    }
    auto commandLine = win32::widen(*commandLineUtf8);
    if (!commandLine) {
        return propagate(commandLine.error());
    }
    const std::wstring application = request.executable.native();
    const std::wstring directory = launcher::workingDirectoryOf(request).native();
    std::string executableUtf8;
    try {
        executableUtf8 = core::pathToUtf8(request.executable);
    } catch (...) {
        executableUtf8 = "?";
    }

    // The environment: Retroverse's own, unless the request changes it.
    std::wstring environmentBlock;
    if (!request.environment.empty()) {
        for (const launcher::EnvironmentVariable& variable :
             launcher::mergeEnvironment(currentEnvironment(), request.environment)) {
            auto entry = win32::widen(variable.name + "=" + variable.value);
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

    // Standard output and error appended to the output log; only these handles are inherited.
    UniqueHandle output;
    UniqueHandle input;
    AttributeList attributes;
    std::array<HANDLE, 2> inheritedHandles{};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_PROCESS_GROUP;
    if (request.hideConsoleWindow) {
        flags |= CREATE_NO_WINDOW;
    }
    BOOL inheritHandles = FALSE;
    if (!request.outputLog.empty()) {
        if (auto opened = openOutputLog(request.outputLog, *commandLineUtf8, output, input); !opened) {
            return propagate(opened.error());
        }
        if (auto initialized = attributes.initialize(1); !initialized) {
            return propagate(initialized.error());
        }
        inheritedHandles = {output.get(), input.get()};
        if (!UpdateProcThreadAttribute(attributes.list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inheritedHandles.data(), sizeof(HANDLE) * inheritedHandles.size(), nullptr,
                                       nullptr)) {
            return makeError(ErrorCode::PlatformError,
                             "UpdateProcThreadAttribute failed: " + win32::lastErrorMessage());
        }
        startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.get();
        startup.StartupInfo.hStdOutput = output.get();
        startup.StartupInfo.hStdError = output.get();
        startup.lpAttributeList = attributes.list();
        flags |= EXTENDED_STARTUPINFO_PRESENT;
        inheritHandles = TRUE;
    }

    // The job is ready, its completion port included, before the process exists: nothing of the tree is missed.
    UniqueHandle job;
    UniqueHandle port;
    std::string jobError;
    createJob(job, port, jobError);

    // Windows reports some images it cannot run (DOS and 16-bit programs) with a modal message box in the calling
    // process, which would freeze Retroverse until dismissed: this thread fails instead, for this call only.
    DWORD previousErrorMode = 0;
    const bool errorModeSet =
        SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX,
                           &previousErrorMode) != FALSE;
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessW(application.c_str(), commandLine->data(), nullptr, nullptr, inheritHandles,
                                        flags, environmentBlock.empty() ? nullptr : environmentBlock.data(),
                                        directory.c_str(), &startup.StartupInfo, &info);
    const DWORD createError = GetLastError();
    if (errorModeSet) {
        SetThreadErrorMode(previousErrorMode, nullptr);
    }
    if (!created) {
        const DWORD error = createError;
        std::string message = "CreateProcessW failed: " + win32::errorMessage(error);
        if (error == ERROR_ELEVATION_REQUIRED) {
            message += "; the program asks for administrator rights, which Retroverse does not request";
        }
        return makeError(ErrorCode::PlatformError, std::move(message), executableUtf8);
    }
    UniqueHandle process(info.hProcess);
    const UniqueHandle thread(info.hThread);

    if (job && !AssignProcessToJobObject(job.get(), process.get())) {
        jobError = "AssignProcessToJobObject failed: " + win32::lastErrorMessage();
        job.reset();
        port.reset();
    }
    bool foregroundAllowed = false;
    std::string foregroundError;
    if (m_options.allowSetForeground) {
        foregroundAllowed = AllowSetForegroundWindow(info.dwProcessId) != FALSE;
        if (!foregroundAllowed) {
            foregroundError = win32::lastErrorMessage();
        }
    }

    auto running = std::make_unique<Win32RunningProcess>(std::move(process), info.dwProcessId, std::move(job),
                                                         std::move(port), std::move(callbacks.wake),
                                                         m_options.pollInterval);
    auto watching = running->startWatching();
    if (watching && callbacks.beforeResume) {
        callbacks.beforeResume(info.dwProcessId);
    }
    if (watching && ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        watching = makeError(ErrorCode::PlatformError, "ResumeThread failed: " + win32::lastErrorMessage());
    }
    if (!watching) {
        // The program never ran (still suspended): end it, so that no frozen process stays behind.
        TerminateProcess(running->processHandle(), ERROR_CANCELLED);
        return makeError(ErrorCode::PlatformError, watching.error().message, executableUtf8);
    }

    m_lastStart = Win32StartInfo{};
    m_lastStart.processId = info.dwProcessId;
    m_lastStart.jobAssigned = running->followsTree();
    m_lastStart.jobError = jobError;
    m_lastStart.foregroundAllowed = foregroundAllowed;
    m_lastStart.foregroundError = foregroundError;
    m_lastStart.commandLine = *commandLineUtf8;
    m_lastStart.startDuration =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin);
    spdlog::info("Started process {} in {:.1f} ms ({}; AllowSetForegroundWindow {})", info.dwProcessId,
                 static_cast<double>(m_lastStart.startDuration.count()) / 1000.0,
                 m_lastStart.jobAssigned ? std::string("process tree followed")
                                         : "process tree not followed: " + jobError,
                 foregroundAllowed ? std::string("granted") : "refused: " + foregroundError);
    return std::unique_ptr<launcher::IRunningProcess>(std::move(running));
}

} // namespace rv::platform
