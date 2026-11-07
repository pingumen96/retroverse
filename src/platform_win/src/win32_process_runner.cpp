#include <rv/platform_win/win32_process_runner.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/core/system_probe.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/launcher/launch_guard.hpp>
#include <rv/launcher/launch_request.hpp>
#include <rv/launcher/window_choice.hpp>

#include <rv/platform_win/win32_util.hpp>

// After windows.h: the C runtime's debug reports (disableFailureDialogs).
#include <crtdbg.h>
// After windows.h (win32_util.hpp): NtQuerySystemInformation and SYSTEM_PROCESS_INFORMATION (ntdll).
#include <winternl.h>

#include <dwmapi.h>
#include <objbase.h>
// ShellExecuteExW and SHQueryUserNotificationState (I11 step 13); the system's process snapshot.
#include <shellapi.h>
#include <tlhelp32.h>

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
/// FollowWindow (I11 step 13): processes adopted by their windows at most, windows refused remembered at most, and how
/// far up a candidate's ancestors are looked at.
constexpr std::size_t kMaxAdoptedProcesses = 16;
constexpr std::size_t kMaxRefusedWindows = 4096;
constexpr int kMaxAncestors = 8;

/// NtQuerySystemInformation's answer when the buffer is too small (ntstatus.h, which clashes with windows.h).
constexpr NTSTATUS kStatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004UL);
/// The first buffer for the system's process list, and how many times it grows before the list counts as unreadable.
constexpr ULONG kProcessListInitialBytes = 512u * 1024u;
constexpr int kProcessListAttempts = 8;

/// The head of an entry of the system's process list (SystemProcessInformation) as Windows lays it out: winternl.h hides
/// the fields before ImageName in Reserved1, among them the creation time, the one GetProcessTimes reads.
struct SystemProcessHead {
    ULONG nextEntryOffset;
    ULONG numberOfThreads;
    LARGE_INTEGER workingSetPrivateSize;
    ULONG hardFaultCount;
    ULONG numberOfThreadsHighWatermark;
    ULONGLONG cycleTime;
    LARGE_INTEGER createTime;
    LARGE_INTEGER userTime;
    LARGE_INTEGER kernelTime;
    UNICODE_STRING imageName;
    KPRIORITY basePriority;
    HANDLE uniqueProcessId;
};
static_assert(offsetof(SystemProcessHead, numberOfThreads) == offsetof(SYSTEM_PROCESS_INFORMATION, NumberOfThreads));
static_assert(offsetof(SystemProcessHead, imageName) == offsetof(SYSTEM_PROCESS_INFORMATION, ImageName));
static_assert(offsetof(SystemProcessHead, uniqueProcessId) == offsetof(SYSTEM_PROCESS_INFORMATION, UniqueProcessId));

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

struct WindowCollection {
    const DWORD* processIds = nullptr;
    std::size_t count = 0;
    std::vector<launcher::WindowCandidate>* windows = nullptr;
    /// The windows of every other process instead (FollowWindow's candidates, I11 step 13).
    bool others = false;
};

/// The processes whose windows are never a game's (FollowWindow, I11 step 13): the shell, error reporting, consoles and
/// the elevation prompt.
constexpr std::array<std::string_view, 9> kNeverGameImages{
    "explorer.exe",     "WerFault.exe",       "WerFaultSecure.exe", "ApplicationFrameHost.exe", "conhost.exe",
    "OpenConsole.exe", "WindowsTerminal.exe", "consent.exe",        "ShellExperienceHost.exe"};

/// FILETIME ticks of a process's creation; 0 when they cannot be read.
std::uint64_t creationTicks(HANDLE process) noexcept {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    return GetProcessTimes(process, &creation, &exit, &kernel, &user) ? ticks(creation) : 0;
}

/// The system's processes now (a Toolhelp snapshot): each one's parent and file name.
class ProcessTable {
public:
    struct Entry {
        std::uint32_t parent = 0;
        std::string image;
    };

    static ProcessTable take() {
        ProcessTable table;
        const UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) {
            return table;
        }
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
            table.m_entries.emplace_back(static_cast<std::uint32_t>(entry.th32ProcessID),
                                         Entry{static_cast<std::uint32_t>(entry.th32ParentProcessID),
                                               core::wideToUtf8Lossy(entry.szExeFile)});
        }
        return table;
    }

    [[nodiscard]] const Entry* find(std::uint32_t processId) const noexcept {
        const auto found = std::find_if(m_entries.begin(), m_entries.end(),
                                        [&](const auto& entry) { return entry.first == processId; });
        return found == m_entries.end() ? nullptr : &found->second;
    }

private:
    std::vector<std::pair<std::uint32_t, Entry>> m_entries;
};

/// The window classes of a console's window: the classic console host, a pseudo console, Windows Terminal.
bool isConsoleClass(std::wstring_view name) noexcept {
    return name == L"ConsoleWindowClass" || name == L"PseudoConsoleWindow" || name == L"CASCADIA_HOSTING_WINDOW_CLASS";
}

/// Every visible top-level window of the tree with a non-empty rectangle, described for launcher::chooseGameWindow()
/// (I11 step 11). GetWindowTextW reads another process's caption without sending it a message, so a hung emulator
/// never blocks the watcher.
BOOL CALLBACK collectVisibleWindow(HWND window, LPARAM parameter) {
    auto* collection = reinterpret_cast<WindowCollection*>(parameter);
    if (!IsWindowVisible(window)) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    const bool listed = std::find(collection->processIds, collection->processIds + collection->count, processId) !=
                        collection->processIds + collection->count;
    if (listed == collection->others) {
        return TRUE;
    }
    RECT rect{};
    if (!GetWindowRect(window, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) {
        return TRUE;
    }
    launcher::WindowCandidate candidate;
    candidate.handle = reinterpret_cast<std::uintptr_t>(window);
    candidate.processId = processId;
    std::array<wchar_t, 256> className{};
    const int classLength = GetClassNameW(window, className.data(), static_cast<int>(className.size()));
    const std::wstring_view classView(className.data(), static_cast<std::size_t>(std::max(classLength, 0)));
    candidate.className = core::wideToUtf8Lossy(classView);
    std::array<wchar_t, 512> title{};
    const int titleLength = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    candidate.title =
        core::wideToUtf8Lossy(std::wstring_view(title.data(), static_cast<std::size_t>(std::max(titleLength, 0))));
    RECT client{};
    if (GetClientRect(window, &client)) {
        candidate.width = static_cast<int>(client.right - client.left);
        candidate.height = static_cast<int>(client.bottom - client.top);
    }
    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    candidate.tool = (exStyle & WS_EX_TOOLWINDOW) != 0;
    candidate.owned = GetWindow(window, GW_OWNER) != nullptr;
    candidate.console = isConsoleClass(classView);
    DWORD cloaked = 0;
    candidate.cloaked =
        SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
    candidate.minimized = IsIconic(window) != FALSE;
    collection->windows->push_back(std::move(candidate));
    return TRUE;
}

/// FollowWindow's settings for a running process (I11 step 13, ADR-0020 §4).
struct FollowOptions {
    bool enabled = false;
    /// LaunchRequest::launcherProcesses.
    std::vector<std::string> launcherProcesses;
    /// The system time (FILETIME ticks, GetSystemTimeAsFileTime: the clock process creation times are read on) just
    /// before the launch: a process created earlier is never adopted.
    std::uint64_t launchTicks = 0;
};

/// A started process followed by a watcher thread (or, for a protocol activation, none: the windows of the processes
/// adopted). Never terminates it (once it has run).
class Win32RunningProcess final : public launcher::IRunningProcess {
public:
    Win32RunningProcess(UniqueHandle process, std::uint32_t processId, UniqueHandle job, UniqueHandle port,
                        std::function<void()> wake, std::chrono::milliseconds pollInterval,
                        launcher::WindowRules windowRules, FollowOptions follow = {})
        : m_process(std::move(process)), m_processId(processId), m_job(std::move(job)), m_port(std::move(port)),
          m_stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)), m_wake(std::move(wake)),
          m_pollMilliseconds(static_cast<DWORD>(std::clamp<std::int64_t>(pollInterval.count(), 1, 10000))),
          m_follow(std::move(follow)), m_follower(std::move(windowRules)) {}

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
            if (m_follow.enabled) {
                watchFollowed();
                return;
            }
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

    /// FollowWindow (I11 step 13, ADR-0020 §4): the started process and its tree when there is one, and the processes
    /// adopted by their windows (adoptWindows()), until the game's window has shown and every one of them has exited
    /// (TreeEmpty), or watching stops. Before a window has shown, an empty tree is a launcher that handed over: the
    /// windows are still looked for.
    void watchFollowed() {
        if (m_processId != 0) {
            m_known.insert(m_processId);
        }
        while (!m_stopping.load()) {
            if (m_port) {
                drainJobMessages(m_pollMilliseconds);
            } else {
                std::array<HANDLE, 2> handles{};
                DWORD count = 0;
                if (m_process && !m_mainExited) {
                    handles[count++] = m_process.get();
                }
                handles[count++] = m_stopEvent.get();
                if (WaitForMultipleObjects(count, handles.data(), FALSE, m_pollMilliseconds) == WAIT_FAILED) {
                    reportFailure("waiting for the processes failed: " + lastErrorMessage());
                    return;
                }
            }
            if (m_stopping.load() || m_treeEmptyReported) {
                return;
            }
            if (m_process && !m_mainExited && WaitForSingleObject(m_process.get(), 0) == WAIT_OBJECT_0) {
                if (m_port) {
                    drainJobMessages(0); // the processes it started first
                }
                reportMainExit();
            }
            reportAdoptedExits();
            adoptWindows();
            checkWindows();
            const bool mainDone = !m_process || m_mainExited;
            const bool treeDone = !m_job || jobEmpty() || onlyConsoleHostsLeft();
            if (m_windowEverShown && mainDone && treeDone && m_adopted.empty()) {
                if (m_job) {
                    concludeTree();
                } else {
                    reportTreeEmpty();
                }
                return;
            }
        }
    }

    /// The exits of the processes adopted, from their handles (no job reports them).
    void reportAdoptedExits() {
        for (auto entry = m_adopted.begin(); entry != m_adopted.end();) {
            if (WaitForSingleObject(entry->handle.get(), 0) != WAIT_OBJECT_0) {
                ++entry;
                continue;
            }
            const LaunchClock::time_point now = LaunchClock::now();
            ProcessEvent event;
            event.kind = ProcessEventKind::ChildExited;
            event.observedAt = now;
            event.processId = entry->processId;
            event.image = entry->image;
            DWORD code = 0;
            if (GetExitCodeProcess(entry->handle.get(), &code)) {
                event.exitCode = code;
            }
            event.occurredAt = exitTimeOf(entry->handle.get(), now);
            m_lastExitTime = std::max(m_lastExitTime, event.occurredAt);
            entry = m_adopted.erase(entry);
            push(std::move(event));
        }
    }

    /// FollowWindow: the processes outside the tree whose window could be the game's (a window rankWindow() does not
    /// exclude) are followed from now on, reported as ChildStarted (ADR-0020 §4), when they were created
    /// since the launch and were started, directly or through processes created since the launch, by a process
    /// followed, by Retroverse or by one of the request's launcher processes; and are none of those launcher processes,
    /// Retroverse, Explorer or a console. A window refused is not looked at again.
    void adoptWindows() {
        if (m_adopted.size() >= kMaxAdoptedProcesses) {
            return;
        }
        std::vector<DWORD> followed(m_known.begin(), m_known.end());
        followed.push_back(GetCurrentProcessId());
        std::vector<launcher::WindowCandidate> windows;
        WindowCollection collection{followed.data(), followed.size(), &windows, /*others*/ true};
        EnumWindows(&collectVisibleWindow, reinterpret_cast<LPARAM>(&collection));
        std::optional<ProcessTable> table;
        for (const launcher::WindowCandidate& window : windows) {
            const launcher::WindowRank rank = launcher::rankWindow(window, m_follower.rules());
            if (rank == launcher::WindowRank::Excluded || m_refusedWindows.contains(window.handle) ||
                m_known.contains(window.processId)) {
                continue;
            }
            if (!table) {
                table = ProcessTable::take();
            }
            if (!adopt(window.processId, *table)) {
                if (m_refusedWindows.size() >= kMaxRefusedWindows) {
                    m_refusedWindows.clear();
                }
                m_refusedWindows.insert(window.handle);
            }
            if (m_adopted.size() >= kMaxAdoptedProcesses) {
                return;
            }
        }
    }

    /// Adopts `processId` when adoptWindows()' rules allow it; false when they do not.
    bool adopt(std::uint32_t processId, const ProcessTable& table) {
        UniqueHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId));
        if (!handle) {
            return false;
        }
        const std::uint64_t created = creationTicks(handle.get());
        if (created == 0 || created < m_follow.launchTicks) {
            return false;
        }
        const std::wstring path = imagePath(handle.get());
        const std::string image = fileNameOf(path);
        if (image.empty() || isConsoleHost(path) || neverGame(image) || launcherProcess(image) ||
            !startedSinceLaunch(processId, created, table)) {
            return false;
        }
        m_known.insert(processId);
        m_adopted.push_back(Tracked{processId, std::move(handle), image, false});
        spdlog::info("Launch: process {} ({}) shows a window: followed as the game's", processId, image);
        const LaunchClock::time_point now = LaunchClock::now();
        ProcessEvent event;
        event.kind = ProcessEventKind::ChildStarted;
        event.observedAt = now;
        event.occurredAt = now;
        event.processId = processId;
        event.image = image;
        push(std::move(event));
        return true;
    }

    [[nodiscard]] static bool neverGame(std::string_view image) {
        return std::any_of(kNeverGameImages.begin(), kNeverGameImages.end(),
                           [&](std::string_view name) { return core::matchesWildcard(image, name); });
    }

    [[nodiscard]] bool launcherProcess(std::string_view image) const {
        return std::any_of(m_follow.launcherProcesses.begin(), m_follow.launcherProcesses.end(),
                           [&](const std::string& pattern) { return core::matchesWildcard(image, pattern); });
    }

    /// Whether `processId` (created at `created`) descends from a process followed, from Retroverse or from a launcher
    /// process, through processes created since the launch. A parent whose id was given to a newer process is known
    /// by its id alone (it may have been followed).
    [[nodiscard]] bool startedSinceLaunch(std::uint32_t processId, std::uint64_t created, const ProcessTable& table) {
        std::uint32_t current = processId;
        std::uint64_t currentCreated = created;
        for (int depth = 0; depth < kMaxAncestors; ++depth) {
            const ProcessTable::Entry* entry = table.find(current);
            if (entry == nullptr || entry->parent == 0) {
                return false;
            }
            const std::uint32_t parent = entry->parent;
            if (parent == GetCurrentProcessId() || m_known.contains(parent)) {
                return true;
            }
            const ProcessTable::Entry* parentEntry = table.find(parent);
            if (parentEntry == nullptr) {
                return false; // gone, and never followed
            }
            const UniqueHandle parentHandle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent));
            const std::uint64_t parentCreated = parentHandle ? creationTicks(parentHandle.get()) : 0;
            if (parentCreated == 0 || parentCreated > currentCreated) {
                return false; // unreadable, or the id now names a newer process
            }
            if (launcherProcess(parentEntry->image)) {
                return true;
            }
            if (parentCreated < m_follow.launchTicks) {
                return false; // an older process that is none of those: not this launch's
            }
            current = parent;
            currentCreated = parentCreated;
        }
        return false;
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
            if (m_follow.enabled) {
                break; // FollowWindow ends with the processes adopted too (watchFollowed)
            }
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
        m_known.insert(processId);
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

    /// Reports WindowShown / WindowsGone when the tree's visibility changed since the last check, and WindowChanged when
    /// the game's window followed (launcher::WindowFollower) moved while the tree stayed visible (I11 step 11).
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
        } else if (!m_mainExited && m_processId != 0) {
            processIds[count++] = m_processId;
        }
        // FollowWindow: the processes adopted (I11 step 13).
        for (const Tracked& adopted : m_adopted) {
            if (count < processIds.size()) {
                processIds[count++] = static_cast<DWORD>(adopted.processId);
            }
        }
        std::vector<launcher::WindowCandidate> windows;
        WindowCollection collection{processIds.data(), count, &windows};
        if (count > 0) {
            EnumWindows(&collectVisibleWindow, reinterpret_cast<LPARAM>(&collection));
        }
        const LaunchClock::time_point now = LaunchClock::now();
        // How long each window has been shown: windows no longer there are forgotten.
        std::erase_if(m_firstSeen, [&](const auto& seen) {
            return std::none_of(windows.begin(), windows.end(),
                                [&](const launcher::WindowCandidate& window) { return window.handle == seen.first; });
        });
        for (launcher::WindowCandidate& window : windows) {
            auto seen = std::find_if(m_firstSeen.begin(), m_firstSeen.end(),
                                     [&](const auto& entry) { return entry.first == window.handle; });
            if (seen == m_firstSeen.end()) {
                m_firstSeen.emplace_back(window.handle, now);
                seen = std::prev(m_firstSeen.end());
            }
            window.shownFor = std::chrono::duration_cast<std::chrono::milliseconds>(now - seen->second);
        }
        const std::optional<std::uintptr_t> changed = m_follower.update(windows, now);
        const bool visible = !windows.empty();
        if (visible) {
            m_windowEverShown = true;
            checkExclusiveFullscreen(processIds.data(), count);
        }
        if (changed) {
            const auto followed = std::find_if(windows.begin(), windows.end(), [&](const auto& window) {
                return window.handle == *changed;
            });
            if (followed != windows.end()) {
                spdlog::debug("Launch: the game's window followed is now a {} x {} window of class {} (of {} shown)",
                              followed->width, followed->height, followed->className, windows.size());
            }
        }
        ProcessEvent event;
        event.observedAt = now;
        event.occurredAt = now;
        if (visible != m_windowVisible) {
            m_windowVisible = visible;
            event.kind = visible ? ProcessEventKind::WindowShown : ProcessEventKind::WindowsGone;
            event.window = visible ? m_follower.current() : 0;
            push(std::move(event));
            return;
        }
        if (visible && changed) {
            event.kind = ProcessEventKind::WindowChanged;
            event.window = *changed;
            push(std::move(event));
        }
    }

    /// ADR-0020 §5: once a session, a window of the processes followed holds the foreground while the shell reports a
    /// Direct3D game in exclusive fullscreen.
    void checkExclusiveFullscreen(const DWORD* processIds, std::size_t count) {
        if (m_exclusiveReported) {
            return;
        }
        const HWND foreground = GetForegroundWindow();
        DWORD foregroundProcess = 0;
        if (foreground == nullptr || GetWindowThreadProcessId(foreground, &foregroundProcess) == 0 ||
            std::find(processIds, processIds + count, foregroundProcess) == processIds + count) {
            return;
        }
        QUERY_USER_NOTIFICATION_STATE state{};
        if (FAILED(SHQueryUserNotificationState(&state)) || state != QUNS_RUNNING_D3D_FULL_SCREEN) {
            return;
        }
        m_exclusiveReported = true;
        const LaunchClock::time_point now = LaunchClock::now();
        ProcessEvent event;
        event.kind = ProcessEventKind::ExclusiveFullscreen;
        event.observedAt = now;
        event.occurredAt = now;
        event.processId = foregroundProcess;
        event.window = reinterpret_cast<std::uintptr_t>(foreground);
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
    FollowOptions m_follow;
    /// FollowWindow: the processes adopted and running, every process followed so far (ids), the windows refused.
    std::vector<Tracked> m_adopted;
    std::set<std::uint32_t> m_known;
    std::set<std::uintptr_t> m_refusedWindows;
    bool m_windowEverShown = false;
    bool m_exclusiveReported = false;
    launcher::WindowFollower m_follower;
    /// When each visible window of the tree was first seen.
    std::vector<std::pair<std::uintptr_t, LaunchClock::time_point>> m_firstSeen;
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

namespace {

/// How far left of the virtual screen a GUI program started in the background opens its first window: wider than any
/// window it could open there, short of the -32000 where Windows parks minimized windows.
constexpr int kBackgroundWindowOffset = 8000;

} // namespace

void disableFailureDialogs() noexcept {
    static const bool disabled = [] {
        // The loader's and the crash dialogs; programs started without CREATE_DEFAULT_ERROR_MODE inherit the mode, so
        // that one that misses a DLL (STATUS_DLL_NOT_FOUND) or crashes ends with its status.
        SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        // The C runtime's own message boxes, which the error mode does not cover (task-modal, MB_SETFOREGROUND: the
        // run would also block until a timeout). The report modes exist in Debug builds only (no-ops in Release).
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _set_error_mode(_OUT_TO_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        return true;
    }();
    static_cast<void>(disabled);
}

bool backgroundLaunchesRequested() noexcept {
    try {
        const std::optional<std::string> value = launcher::processEnvironmentVariable(kLaunchBackgroundVariable);
        return value.has_value() && !value->empty() && *value != "0";
    } catch (...) {
        return false; // out of memory reading it: the product's behaviour
    }
}

std::uint32_t processCreationFlags(const launcher::LaunchRequest& request, bool background) noexcept {
    // A console program gets a console of its own, as when a GUI program such as ES-DE starts it, windowless on
    // request and in the background (a console window would be activated over whatever the user is doing): never
    // Retroverse's console, whose closing (CTRL_CLOSE_EVENT) or Ctrl+Break would reach every process attached to it.
    // Both flags are ignored for GUI programs. In the background the program inherits this process's error mode
    // (disableFailureDialogs) instead of the default one, which shows the loader's and the crash dialogs.
    DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP |
                  (background ? 0 : CREATE_DEFAULT_ERROR_MODE) |
                  (request.hideConsoleWindow || background ? CREATE_NO_WINDOW : CREATE_NEW_CONSOLE);
    if (!request.outputLog.empty()) {
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    return flags;
}

ProcessIdentity currentProcessIdentity() noexcept {
    ProcessIdentity identity;
    identity.processId = GetCurrentProcessId();
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != FALSE) {
        identity.creationTime = ticks(creation);
    }
    return identity;
}

std::string_view toString(ProcessLiveness liveness) noexcept {
    switch (liveness) {
    case ProcessLiveness::Running:
        return "running";
    case ProcessLiveness::Gone:
        return "gone";
    case ProcessLiveness::Unknown:
        return "unknown";
    }
    return "unknown";
}

Expected<std::optional<ListedProcess>> listedProcess(std::uint32_t processId) {
    ULONG size = kProcessListInitialBytes;
    for (int attempt = 0; attempt < kProcessListAttempts; ++attempt) {
        // 8-byte units: the entries hold 64-bit fields.
        std::vector<std::uint64_t> buffer(size / sizeof(std::uint64_t) + 1);
        const auto bytes = static_cast<ULONG>(buffer.size() * sizeof(std::uint64_t));
        ULONG needed = 0;
        const NTSTATUS status = NtQuerySystemInformation(SystemProcessInformation, buffer.data(), bytes, &needed);
        if (status == kStatusInfoLengthMismatch) {
            size = std::max(needed, bytes) + 64u * 1024u; // processes may start before the next call
            continue;
        }
        if (status < 0) {
            return makeError(ErrorCode::PlatformError,
                             fmt::format("the system's process list could not be read (NTSTATUS 0x{:08X})",
                                         static_cast<std::uint32_t>(status)));
        }
        const auto* base = reinterpret_cast<const std::byte*>(buffer.data());
        std::size_t offset = 0;
        while (offset + sizeof(SystemProcessHead) <= bytes) {
            SystemProcessHead head{};
            std::memcpy(&head, base + offset, sizeof(head));
            if (static_cast<std::uint32_t>(reinterpret_cast<ULONG_PTR>(head.uniqueProcessId)) == processId) {
                return std::optional<ListedProcess>(
                    ListedProcess{static_cast<std::uint64_t>(head.createTime.QuadPart), head.numberOfThreads});
            }
            if (head.nextEntryOffset == 0) {
                break;
            }
            offset += head.nextEntryOffset;
        }
        return std::optional<ListedProcess>();
    }
    return makeError(ErrorCode::PlatformError, "the system's process list kept growing while it was read");
}

ProcessLiveness processLiveness(const ProcessIdentity& process) noexcept {
    const UniqueHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.processId));
    if (!handle) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) {
            return ProcessLiveness::Gone; // no process has the id
        }
        // A process has the id that this user may not open (a service, another user's or session's, a protected
        // one): the system's process list tells its creation time without any access right. It is the owner only
        // when that time is the owner's; a process that took the id later, typically after a reboot, is not.
        try {
            const auto listed = listedProcess(process.processId);
            if (!listed) {
                return ProcessLiveness::Unknown;
            }
            if (!listed->has_value() || (*listed)->creationTime != process.creationTime) {
                return ProcessLiveness::Gone; // it ended meanwhile, or a later process reuses the id
            }
            // A process that exited stays listed, without threads, while handles to it are open.
            return (*listed)->threadCount == 0 ? ProcessLiveness::Gone : ProcessLiveness::Running;
        } catch (...) {
            return ProcessLiveness::Unknown; // no memory for the list
        }
    }
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(handle.get(), &creation, &exit, &kernel, &user) == FALSE) {
        return ProcessLiveness::Unknown;
    }
    if (ticks(creation) != process.creationTime) {
        return ProcessLiveness::Gone; // a later process reuses the id
    }
    DWORD code = 0;
    if (GetExitCodeProcess(handle.get(), &code) == FALSE) {
        return ProcessLiveness::Unknown;
    }
    // A process that exited stays as an object while handles to it are open: its exit code tells.
    return code == STILL_ACTIVE ? ProcessLiveness::Running : ProcessLiveness::Gone;
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
Win32ProcessRunner::startActivation(const launcher::LaunchRequest& request, launcher::StartCallbacks callbacks) {
    const auto begin = std::chrono::steady_clock::now();
    const auto url = core::utf8ToWide(request.activationUrl);
    if (!url) {
        return propagate(url.error());
    }
    const bool background = m_options.background.value_or(backgroundLaunchesRequested());
    // The window's hand-over comes first, as before a process runs.
    if (callbacks.beforeResume) {
        callbacks.beforeResume(0);
    }
    bool foregroundAllowed = false;
    std::string foregroundError;
    if (background) {
        foregroundError = "not asked for: started in the background";
    } else if (m_options.allowSetForeground) {
        // The game's process is not known yet: any process may take the foreground Retroverse holds.
        foregroundAllowed = AllowSetForegroundWindow(ASFW_ANY) != FALSE;
        if (!foregroundAllowed) {
            foregroundError = lastErrorMessage();
        }
    }
    FILETIME launchTime{};
    GetSystemTimeAsFileTime(&launchTime);
    // ShellExecuteExW may hand the URL to a shell extension: COM on this thread, a single-threaded apartment as the
    // shell expects (a thread already in the other kind keeps it).
    const ComScope com(COINIT_APARTMENTTHREADED);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open";
    info.lpFile = url->c_str();
    info.nShow = background ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        return makeError(ErrorCode::PlatformError, "ShellExecuteExW failed: " + lastErrorMessage(),
                         request.activationUrl);
    }
    // The protocol's handler, when the shell started one (a launcher passing the URL to its running copy): its windows
    // are the launcher's, and the game a process it starts, adopted by its window.
    const UniqueHandle handler(info.hProcess);
    auto running = std::make_unique<Win32RunningProcess>(
        UniqueHandle{}, 0u, UniqueHandle{}, UniqueHandle{}, std::move(callbacks.wake), m_options.pollInterval,
        request.windowRules, FollowOptions{true, request.launcherProcesses, ticks(launchTime)});
    if (auto watching = running->startWatching(); !watching) {
        return makeError(ErrorCode::PlatformError, watching.error().message, request.activationUrl);
    }
    running->setStartedAt(LaunchClock::now());
    try {
        m_lastStart = Win32StartInfo{};
        m_lastStart.foregroundAllowed = foregroundAllowed;
        m_lastStart.foregroundError = foregroundError;
        m_lastStart.background = background;
        m_lastStart.commandLine = request.activationUrl;
        m_lastStart.jobError = "a protocol activation: no process of its own";
        m_lastStart.startDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin);
        spdlog::info("Activated {} in {:.1f} ms (handler process {}); the game's window is followed",
                     launcher::activationScheme(request.activationUrl).value_or("?"),
                     static_cast<double>(m_lastStart.startDuration.count()) / 1000.0,
                     handler ? GetProcessId(handler.get()) : 0u);
    } catch (...) {
        m_lastStart.processId = 0;
    }
    return std::unique_ptr<launcher::IRunningProcess>(std::move(running));
}

Expected<std::unique_ptr<launcher::IRunningProcess>>
Win32ProcessRunner::startProcess(const launcher::LaunchRequest& request, launcher::StartCallbacks callbacks) {
    const auto begin = std::chrono::steady_clock::now();
    // The automation guard (RV_LAUNCH_ALLOWLIST) first: what it refuses never exists, not even suspended.
    if (auto allowed = launcher::checkLaunchGuard(request); !allowed) {
        return propagate(allowed.error());
    }
    if (request.isActivation()) {
        return startActivation(request, std::move(callbacks));
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
    // In the background (automated runs): no console window; a GUI program's first window, when the program lets
    // Windows place it (CW_USEDEFAULT) and show it (its first ShowWindow), opens inactive and left of every monitor,
    // as rv_fake_emulator's background window: still a visible window for the window detection, never one the user
    // sees, and no minimized window in the taskbar. A program that places or activates its window itself is a desktop
    // test's (focus_watch.py reports it).
    const bool background = m_options.background.value_or(backgroundLaunchesRequested());
    const DWORD flags = processCreationFlags(request, background);
    BOOL inheritHandles = FALSE;
    if (background) {
        startup.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW | STARTF_USEPOSITION;
        startup.StartupInfo.wShowWindow = SW_SHOWNOACTIVATE;
        // dwX and dwY are read back as signed coordinates.
        startup.StartupInfo.dwX = static_cast<DWORD>(GetSystemMetrics(SM_XVIRTUALSCREEN) - kBackgroundWindowOffset);
        startup.StartupInfo.dwY = static_cast<DWORD>(GetSystemMetrics(SM_YVIRTUALSCREEN));
        disableFailureDialogs();
    }
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
        startup.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
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

    // FollowWindow adopts only processes created after this moment (I11 step 13).
    FILETIME launchTime{};
    GetSystemTimeAsFileTime(&launchTime);

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
        if (background) {
            foregroundError = "not asked for: started in the background";
        } else if (m_options.allowSetForeground) {
            foregroundAllowed = AllowSetForegroundWindow(info.dwProcessId) != FALSE;
            if (!foregroundAllowed) {
                foregroundError = lastErrorMessage();
            }
        }
        running = std::make_unique<Win32RunningProcess>(
            std::move(process), info.dwProcessId, std::move(job), std::move(port), std::move(callbacks.wake),
            m_options.pollInterval, request.windowRules,
            FollowOptions{request.waitStrategy == launcher::WaitStrategy::FollowWindow, request.launcherProcesses,
                          ticks(launchTime)});
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
        m_lastStart.background = background;
        m_lastStart.commandLine = *commandLineUtf8;
        m_lastStart.startDuration =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin);
        spdlog::info("Started process {} in {:.1f} ms ({}; {})", info.dwProcessId,
                     static_cast<double>(m_lastStart.startDuration.count()) / 1000.0,
                     m_lastStart.jobAssigned ? std::string("process tree followed")
                                             : "process tree not followed: " + jobError,
                     background          ? std::string("in the background: RV_LAUNCH_BACKGROUND")
                     : foregroundAllowed ? std::string("AllowSetForegroundWindow granted")
                                         : "AllowSetForegroundWindow refused: " + foregroundError);
    } catch (...) {
        m_lastStart.processId = info.dwProcessId;
    }
    return std::unique_ptr<launcher::IRunningProcess>(std::move(running));
}

} // namespace rv::platform_win
