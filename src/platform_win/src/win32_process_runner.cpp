#include <rv/platform_win/win32_process_runner.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/launcher/launch_guard.hpp>
#include <rv/launcher/launch_request.hpp>

#include <rv/platform_win/win32_util.hpp>

namespace rv::platform_win {
namespace {

using launcher::LaunchClock;
using launcher::ProcessEvent;
using launcher::ProcessEventKind;

/// Completion keys of the job's port: the job's notifications, and the packet that stops the watcher.
constexpr ULONG_PTR kJobKey = 1;
constexpr ULONG_PTR kStopKey = 2;
/// Running processes of a tree tracked at once for their exit codes and image names (an entry leaves at its process's
/// exit; processes beyond it are still counted by the job).
constexpr std::size_t kMaxTrackedProcesses = 256;
/// Processes of a tree whose windows are looked for.
constexpr std::size_t kMaxWindowProcesses = 64;
/// How long an exiting process may take to be signaled after the job reported its exit.
constexpr DWORD kExitSettleMilliseconds = 2000;
/// How long start() waits for a process it ends before it ran (a suspended process ends at once).
constexpr DWORD kNeverRunExitMilliseconds = 5000;

/// LaunchClock time of a past system time (UTC FILETIME): now, minus how long ago it was.
LaunchClock::time_point toLaunchClock(const FILETIME& systemTime) noexcept {
    const LaunchClock::time_point now = LaunchClock::now();
    FILETIME current{};
    GetSystemTimePreciseAsFileTime(&current);
    const std::uint64_t then = ticks(systemTime);
    const std::uint64_t nowTicks = ticks(current);
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

/// Full path of a process's executable, or empty.
std::wstring imagePath(HANDLE process) {
    std::array<wchar_t, 1024> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        return {};
    }
    return std::wstring(buffer.data(), size);
}

/// The file name of a path, in UTF-8.
std::string fileNameOf(std::wstring_view path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return core::wideToUtf8Lossy(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

/// True for the system's console host (System32\conhost.exe). A console program that gets a console of its own
/// (CREATE_NEW_CONSOLE, CREATE_NO_WINDOW) starts one as its child, inside the job: it serves the console, lives as long
/// as a program is attached to it (possibly one that left the job) and is no part of the emulator.
bool isConsoleHost(const std::wstring& path) {
    static const std::wstring consoleHost = [] {
        std::array<wchar_t, MAX_PATH> directory{};
        const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        if (length == 0 || length >= directory.size()) {
            return std::wstring();
        }
        return std::wstring(directory.data(), length) + L"\\conhost.exe";
    }();
    return !consoleHost.empty() && !path.empty() &&
           CompareStringOrdinal(path.c_str(), static_cast<int>(path.size()), consoleHost.c_str(),
                                static_cast<int>(consoleHost.size()), TRUE) == CSTR_EQUAL;
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
        stopWatching();
        if (m_thread.joinable()) {
            m_thread.join();
        }
        // The handles close now. The job has no JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: its processes keep running.
    }

    [[nodiscard]] std::uint32_t processId() const noexcept override { return m_processId; }
    [[nodiscard]] LaunchClock::time_point startedAt() const noexcept override { return m_startedAt; }
    [[nodiscard]] bool followsTree() const noexcept override { return m_job.valid(); }
    [[nodiscard]] HANDLE processHandle() const noexcept { return m_process.get(); }
    /// The moment the main thread was resumed (start(), before the object is handed out).
    void setStartedAt(LaunchClock::time_point at) noexcept { m_startedAt = at; }

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

    void stopWatching() noexcept override {
        {
            const std::lock_guard lock(m_mutex);
            m_stopped = true; // push() queues nothing and wakes nobody from now on
        }
        m_stopping.store(true);
        if (m_stopEvent) {
            SetEvent(m_stopEvent.get());
        }
        if (m_port) {
            PostQueuedCompletionStatus(m_port.get(), 0, kStopKey, nullptr);
        }
    }

    /// Starts the watcher thread (before the process is resumed).
    [[nodiscard]] Status startWatching() {
        if (!m_stopEvent) {
            return makeError(ErrorCode::PlatformError, "CreateEventW failed: " + lastErrorMessage());
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
        /// The console host of a console program of the tree: followed, never reported (isConsoleHost).
        bool consoleHost = false;
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
                reportFailure("waiting for the process failed: " + lastErrorMessage());
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
            // Job notifications are not guaranteed (JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO can come late, or not at all):
            // the processes left are counted at every wake-up, not only after a quiet poll interval.
            if (jobEmpty() || onlyConsoleHostsLeft()) {
                concludeTree();
                break;
            }
            drainJobMessages(m_pollMilliseconds);
            if (m_treeEmptyReported || m_stopping.load()) {
                break;
            }
            if (const auto now = LaunchClock::now(); now >= nextWindowCheck) {
                checkWindows();
                nextWindowCheck = now + std::chrono::milliseconds(m_pollMilliseconds);
            }
        }
    }

    /// True when the job holds no process any more, by its own count.
    bool jobEmpty() {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        return QueryInformationJobObject(m_job.get(), JobObjectBasicAccountingInformation, &accounting,
                                         sizeof(accounting), nullptr) &&
               accounting.ActiveProcesses == 0;
    }

    /// Reports the tree's end found by counting its processes (jobEmpty, onlyConsoleHostsLeft). The job's counts drop
    /// before the notifications of the last exits are delivered: those are handled first, and the exits of tracked
    /// processes whose notification is still to come are reported from their handles, so that no exit code is lost.
    void concludeTree() {
        drainJobMessages(0);
        if (m_treeEmptyReported || m_stopping.load()) {
            return;
        }
        std::vector<std::uint32_t> ended;
        for (const Tracked& entry : m_tracked) {
            if (!entry.consoleHost && entry.handle && WaitForSingleObject(entry.handle.get(), 0) == WAIT_OBJECT_0) {
                ended.push_back(entry.processId);
            }
        }
        for (const std::uint32_t processId : ended) {
            reportChildExit(processId);
        }
        reportTreeEmpty();
    }

    /// True when the job still holds processes, all of them console hosts of the tree's console programs: such a host
    /// lives on while a program that left the job (breaking away) still uses its console, but no part of the
    /// emulator runs in the tree any more.
    bool onlyConsoleHostsLeft() {
        constexpr std::size_t kListBytes =
            sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + (kMaxWindowProcesses - 1) * sizeof(ULONG_PTR);
        alignas(JOBOBJECT_BASIC_PROCESS_ID_LIST) std::array<std::byte, kListBytes> storage{};
        auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
        if (!QueryInformationJobObject(m_job.get(), JobObjectBasicProcessIdList, list, static_cast<DWORD>(kListBytes),
                                       nullptr) ||
            list->NumberOfProcessIdsInList == 0 ||
            list->NumberOfProcessIdsInList != list->NumberOfAssignedProcesses) {
            return false; // empty (reported by the job), unknown, or longer than this buffer: not only hosts
        }
        for (std::size_t i = 0; i < list->NumberOfProcessIdsInList; ++i) {
            const auto processId = static_cast<std::uint32_t>(list->ProcessIdList[i]);
            const bool host = std::any_of(m_tracked.begin(), m_tracked.end(), [&](const Tracked& entry) {
                return entry.processId == processId && entry.consoleHost && entry.handle &&
                       WaitForSingleObject(entry.handle.get(), 0) == WAIT_TIMEOUT;
            });
            if (!host) {
                return false;
            }
        }
        return true;
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
                    reportFailure("the job's notifications failed: " + errorMessage(error));
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
        bool consoleHost = false;
        if (m_tracked.size() < kMaxTrackedProcesses) {
            UniqueHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId));
            BOOL inJob = FALSE;
            if (handle && (!IsProcessInJob(handle.get(), m_job.get(), &inJob) || !inJob)) {
                handle.reset(); // the id was reused by a process outside the tree
            }
            if (handle) {
                const std::wstring path = imagePath(handle.get());
                event.image = fileNameOf(path);
                consoleHost = isConsoleHost(path);
            } else {
                m_untrackedExit = true;
            }
            m_tracked.push_back(Tracked{processId, std::move(handle), event.image, consoleHost});
        } else {
            m_untrackedExit = true;
        }
        if (!consoleHost) {
            push(std::move(event));
        }
    }

    void reportChildExit(std::uint32_t processId) {
        const LaunchClock::time_point now = LaunchClock::now();
        ProcessEvent event;
        event.kind = ProcessEventKind::ChildExited;
        event.observedAt = now;
        event.occurredAt = now;
        event.processId = processId;
        // The latest entry of that id: once a process's exit is handled its id may be given to a new process of the
        // tree, which then has its own entry.
        const auto tracked = std::find_if(m_tracked.rbegin(), m_tracked.rend(),
                                          [&](const Tracked& entry) { return entry.processId == processId; });
        if (tracked != m_tracked.rend() && tracked->consoleHost) {
            m_tracked.erase(std::next(tracked).base()); // gone with the last program attached to its console
            return;
        }
        if (tracked != m_tracked.rend() && tracked->handle) {
            event.image = tracked->image;
            DWORD code = 0;
            // The job reports the exit while the process is being torn down, a moment before its handle is
            // signaled and its exit code final. Watching may stop meanwhile.
            const std::array<HANDLE, 2> handles{tracked->handle.get(), m_stopEvent.get()};
            if (WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE,
                                       kExitSettleMilliseconds) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(tracked->handle.get(), &code)) {
                event.exitCode = code;
            }
            event.occurredAt = exitTimeOf(tracked->handle.get(), now);
        } else {
            m_untrackedExit = true;
        }
        if (tracked != m_tracked.rend()) {
            m_tracked.erase(std::next(tracked).base()); // only running processes are tracked: closes its handle
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
        const std::lock_guard lock(m_mutex);
        if (m_stopped) {
            return;
        }
        m_queue.push_back(std::move(event));
        // Under the lock: once stopWatching() has returned, the caller is never woken again.
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
    LaunchClock::time_point m_startedAt{};
    std::atomic<bool> m_stopping{false};
    std::mutex m_mutex;
    std::vector<ProcessEvent> m_queue; // guarded by m_mutex
    bool m_stopped = false;            // guarded by m_mutex
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
        error = "CreateJobObjectW failed: " + lastErrorMessage();
        return;
    }
    // No JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: closing the handle (Retroverse exiting) leaves the processes running.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!SetInformationJobObject(newJob.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        error = "SetInformationJobObject (limits) failed: " + lastErrorMessage();
        return;
    }
    UniqueHandle newPort(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
    if (!newPort) {
        error = "CreateIoCompletionPort failed: " + lastErrorMessage();
        return;
    }
    JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
    association.CompletionKey = reinterpret_cast<PVOID>(kJobKey);
    association.CompletionPort = newPort.get();
    if (!SetInformationJobObject(newJob.get(), JobObjectAssociateCompletionPortInformation, &association,
                                 sizeof(association))) {
        error = "SetInformationJobObject (completion port) failed: " + lastErrorMessage();
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
        return makeError(ErrorCode::IoError, "the output log cannot be opened: " + lastErrorMessage(),
                         core::pathToUtf8Lossy(path));
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
        return makeError(ErrorCode::IoError, "the NUL device cannot be opened: " + lastErrorMessage());
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
                             "InitializeProcThreadAttributeList failed: " + lastErrorMessage());
        }
        m_storage.resize(size);
        if (!InitializeProcThreadAttributeList(list(), attributeCount, 0, &size)) {
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
        variables.push_back(launcher::EnvironmentVariable{core::wideToUtf8Lossy(text.substr(0, equals)),
                                                          core::wideToUtf8Lossy(text.substr(equals + 1))});
    }
    FreeEnvironmentStringsW(block);
    return variables;
}

std::uint32_t processCreationFlags(const launcher::LaunchRequest& request) noexcept {
    // A console program gets a console of its own, as when a GUI program such as ES-DE starts it, windowless on
    // request: never Retroverse's console, whose closing (CTRL_CLOSE_EVENT) or Ctrl+Break would reach every process
    // attached to it. Both flags are ignored for GUI programs.
    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_DEFAULT_ERROR_MODE | CREATE_NEW_PROCESS_GROUP |
                  (request.hideConsoleWindow ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE);
    if (!request.outputLog.empty()) {
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    return flags;
}

Expected<std::unique_ptr<launcher::IRunningProcess>> Win32ProcessRunner::start(const launcher::LaunchRequest& request,
                                                                               launcher::StartCallbacks callbacks) {
    // Nothing escapes: startProcess() ends a process that never ran before an exception leaves it.
    try {
        return startProcess(request, std::move(callbacks));
    } catch (const std::exception& e) {
        return makeError(ErrorCode::PlatformError, std::string("the program could not be started: ") + e.what());
    } catch (...) {
        return makeError(ErrorCode::PlatformError, "the program could not be started: unexpected error");
    }
}

Expected<std::unique_ptr<launcher::IRunningProcess>>
Win32ProcessRunner::startProcess(const launcher::LaunchRequest& request, launcher::StartCallbacks callbacks) {
    const auto begin = std::chrono::steady_clock::now();
    // The automation guard (RV_LAUNCH_ALLOWLIST) first: what it refuses never exists, not even suspended.
    if (auto allowed = launcher::checkLaunchGuard(request); !allowed) {
        return propagate(allowed.error());
    }
    const auto commandLineUtf8 = launcher::commandLineOf(request);
    if (!commandLineUtf8) {
        return propagate(commandLineUtf8.error());
    }
    auto commandLine = core::utf8ToWide(*commandLineUtf8);
    if (!commandLine) {
        return propagate(commandLine.error());
    }
    const std::wstring application = request.executable.native();
    const std::wstring directory = launcher::workingDirectoryOf(request).native();
    const std::string executableUtf8 = core::pathToUtf8Lossy(request.executable);

    // The environment: Retroverse's own, unless the request changes it.
    std::wstring environmentBlock;
    if (!request.environment.empty()) {
        for (const launcher::EnvironmentVariable& variable :
             launcher::mergeEnvironment(currentEnvironment(), request.environment)) {
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

    // Standard output and error appended to the output log; only these handles are inherited.
    UniqueHandle output;
    UniqueHandle input;
    AttributeList attributes;
    std::array<HANDLE, 2> inheritedHandles{};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOW);
    const DWORD flags = processCreationFlags(request);
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
                             "UpdateProcThreadAttribute failed: " + lastErrorMessage());
        }
        startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.get();
        startup.StartupInfo.hStdOutput = output.get();
        startup.StartupInfo.hStdError = output.get();
        startup.lpAttributeList = attributes.list(); // EXTENDED_STARTUPINFO_PRESENT is in the flags
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
        std::string message = "CreateProcessW failed: " + errorMessage(error);
        if (error == ERROR_ELEVATION_REQUIRED) {
            message += "; the program asks for administrator rights, which Retroverse does not request";
        }
        return makeError(ErrorCode::PlatformError, std::move(message), executableUtf8);
    }
    UniqueHandle process(info.hProcess);
    const UniqueHandle thread(info.hThread);
    std::unique_ptr<Win32RunningProcess> running;
    // Until its main thread is resumed the program has not run: every failure from here on, an exception from the
    // caller's beforeResume or an allocation included, ends it, so that no frozen process stays behind.
    const auto endNeverRun = [&]() noexcept {
        if (running) {
            running->stopWatching();
        }
        const HANDLE handle = running ? running->processHandle() : process.get();
        if (handle != nullptr) {
            TerminateProcess(handle, ERROR_CANCELLED);
            WaitForSingleObject(handle, kNeverRunExitMilliseconds);
        }
    };
    bool foregroundAllowed = false;
    std::string foregroundError;
    try {
        if (job && !AssignProcessToJobObject(job.get(), process.get())) {
            jobError = "AssignProcessToJobObject failed: " + lastErrorMessage();
            job.reset();
            port.reset();
        }
        if (m_options.allowSetForeground) {
            foregroundAllowed = AllowSetForegroundWindow(info.dwProcessId) != FALSE;
            if (!foregroundAllowed) {
                foregroundError = lastErrorMessage();
            }
        }
        running = std::make_unique<Win32RunningProcess>(std::move(process), info.dwProcessId, std::move(job),
                                                        std::move(port), std::move(callbacks.wake),
                                                        m_options.pollInterval);
        if (auto watching = running->startWatching(); !watching) {
            endNeverRun();
            return makeError(ErrorCode::PlatformError, watching.error().message, executableUtf8);
        }
        if (callbacks.beforeResume) {
            callbacks.beforeResume(info.dwProcessId);
        }
    } catch (...) {
        endNeverRun();
        throw; // start() reports it
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const std::string error = lastErrorMessage();
        endNeverRun();
        return makeError(ErrorCode::PlatformError, "ResumeThread failed: " + error, executableUtf8);
    }
    running->setStartedAt(LaunchClock::now());

    // The program runs: what follows is bookkeeping for logs and reports, which must not lose the running process.
    try {
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
    } catch (...) {
        m_lastStart.processId = info.dwProcessId;
    }
    return std::unique_ptr<launcher::IRunningProcess>(std::move(running));
}

} // namespace rv::platform_win
