#include <rv/platform_win/directory_watcher.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>
#include <rv/platform_win/win32_util.hpp>

// After windows.h (win32_util.hpp): the device notifications.
#include <dbt.h>

namespace rv::platform_win {

namespace {

using Clock = std::chrono::steady_clock;

/// The notification buffer of a root: 64 KiB, the most a network share takes (ERROR_INVALID_PARAMETER beyond).
constexpr DWORD kBufferBytes = 64 * 1024;
constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
/// How long the thread's end waits for the cancelled reads' completions.
constexpr std::chrono::milliseconds kDrainBound{2000};
/// A re-arm that waits for a cancelled read's completion looks again this soon.
constexpr std::chrono::milliseconds kRetrySoon{50};

constexpr wchar_t kWindowClass[] = L"RetroverseDirectoryWatcher";

/// The size and last write time of a polled entry; nothing when it is not there.
struct EntryFacts {
    bool directory = false;
    std::uint64_t size = 0;
    std::uint64_t written = 0;
    bool operator==(const EntryFacts&) const = default;
};

[[nodiscard]] std::optional<EntryFacts> entryFacts(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(core::extendedLengthPath(path).c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    EntryFacts facts;
    facts.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    facts.size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    facts.written = ticks(data.ftLastWriteTime);
    return facts;
}

[[nodiscard]] bool isFolder(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(core::extendedLengthPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/// The final path of an open handle (GetFinalPathNameByHandleW: no disk read, the name the file system holds).
[[nodiscard]] std::wstring finalPathOf(HANDLE handle) {
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD length = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
                                                       FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) {
            return {};
        }
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(length + 1);
    }
    return {};
}

[[nodiscard]] bool refusesNotifications(DWORD error) noexcept {
    return error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER;
}

} // namespace

/// What the thread and the callers share, under `mutex`.
struct DirectoryWatcher::Shared {
    mutable std::mutex mutex;
    std::condition_variable started;
    DirectoryWatcher::Options options;
    /// Auto-reset: a watch() or the stop.
    HANDLE command = nullptr;
    bool stopping = false;
    /// The roots asked for, and how many times watch() was called; the thread's generation once it armed them.
    std::vector<core::WatchRoot> roots;
    std::uint64_t generation = 0;
    std::uint64_t settledGeneration = 0;
    std::vector<core::Change> changes;
    /// Roots whose changes did not fit (kMaxPendingChanges): one Overflow each at the next take().
    std::vector<std::size_t> overflowed;
    std::vector<core::RootStatus> status;
    std::vector<HANDLE> handles;
    HWND window = nullptr;
    bool windowReady = false;

    /// Queues `change` (the thread's): false when it did not fit.
    void push(core::Change change) {
        if (changes.size() >= core::kMaxPendingChanges) {
            if (std::find(overflowed.begin(), overflowed.end(), change.root) == overflowed.end()) {
                overflowed.push_back(change.root);
            }
            return;
        }
        changes.push_back(std::move(change));
    }
};

namespace {

/// The watcher's thread: the roots, their handles and reads, the window.
class Engine {
public:
    explicit Engine(std::shared_ptr<DirectoryWatcher::Shared> shared)
        : m_shared(std::move(shared)), m_options(m_shared->options) {}

    void run() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        createWindow();
        while (true) {
            std::optional<std::vector<core::WatchRoot>> roots;
            std::uint64_t generation = 0;
            {
                const std::lock_guard lock(m_shared->mutex);
                if (m_shared->stopping) {
                    break;
                }
                if (m_shared->generation != m_generation) {
                    roots = m_shared->roots;
                    generation = m_shared->generation;
                }
            }
            if (roots) {
                replaceRoots(std::move(*roots), generation);
            }
            const Clock::time_point now = Clock::now();
            tick(now);
            flush();
            const DWORD timeout = timeoutUntilNext(Clock::now());
            const DWORD waited = MsgWaitForMultipleObjectsEx(1, &m_shared->command, timeout, QS_ALLINPUT,
                                                             MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);
            if (waited == WAIT_OBJECT_0 + 1) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    DispatchMessageW(&message);
                }
            }
            flush();
        }
        for (auto& root : m_roots) {
            closeRoot(*root, true);
            retire(std::move(root));
        }
        m_roots.clear();
        drain();
        if (m_window != nullptr) {
            DestroyWindow(m_window);
            m_window = nullptr;
        }
        flush();
    }

    /// WM_DEVICECHANGE of a registered handle (the window procedure's).
    LRESULT deviceChange(WPARAM event, LPARAM data) {
        if (data == 0) {
            return TRUE;
        }
        const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(data);
        if (header->dbch_devicetype != DBT_DEVTYP_HANDLE) {
            return TRUE;
        }
        const auto* broadcast = reinterpret_cast<const DEV_BROADCAST_HANDLE*>(data);
        Root* root = rootOf(*broadcast);
        if (root == nullptr) {
            return TRUE;
        }
        switch (event) {
        case DBT_DEVICEQUERYREMOVE:
            // The volume is asked to go: nothing of ours may hold it. The registration stays for the answer.
            spdlog::info("Library watcher: {}'s volume is asked to go: its folder is let go", root->spec.name);
            closeRoot(*root, false);
            root->removalPending = true;
            root->removalSince = Clock::now();
            lose(*root, "volume-removal", true);
            break;
        case DBT_DEVICEQUERYREMOVEFAILED:
            spdlog::info("Library watcher: {}'s volume stays: watched again", root->spec.name);
            unregister(*root);
            root->removalPending = false;
            root->nextCheck = Clock::now();
            break;
        case DBT_DEVICEREMOVEPENDING:
        case DBT_DEVICEREMOVECOMPLETE:
            spdlog::info("Library watcher: {}'s volume went", root->spec.name);
            closeRoot(*root, true);
            root->removalPending = false;
            lose(*root, "volume-removed", true);
            break;
        default:
            break;
        }
        flush();
        return TRUE;
    }

private:
    struct Root {
        Engine* engine = nullptr;
        std::size_t index = 0;
        core::WatchRoot spec;
        HANDLE handle = INVALID_HANDLE_VALUE;
        /// The last handle's value, closed or not: what a device notification about it names.
        HANDLE lastHandle = nullptr;
        HDEVNOTIFY notify = nullptr;
        OVERLAPPED overlapped{};
        std::vector<DWORD> buffer = std::vector<DWORD>(kBufferBytes / sizeof(DWORD));
        /// A read is issued and its completion routine has not run yet: the buffer and OVERLAPPED are the kernel's.
        bool ioPending = false;
        bool retired = false;
        core::WatchMode mode = core::WatchMode::Arming;
        bool removalPending = false;
        Clock::time_point removalSince{};
        Clock::time_point nextCheck{};
        std::wstring finalPath;
        std::map<std::filesystem::path, std::optional<EntryFacts>> snapshot;
        std::string reason;
        std::uint64_t lost = 0;
        std::uint64_t back = 0;
    };

    void createWindow() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Engine::windowProcedure;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kWindowClass;
        if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            spdlog::warn("Library watcher: no window for device notifications ({}): a watched drive cannot be ejected "
                         "while Retroverse runs",
                         lastErrorMessage());
        }
        m_window = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        if (m_window == nullptr) {
            spdlog::warn("Library watcher: no window for device notifications ({}): a watched drive cannot be ejected "
                         "while Retroverse runs",
                         lastErrorMessage());
        } else {
            SetWindowLongPtrW(m_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        }
        const std::lock_guard lock(m_shared->mutex);
        m_shared->window = m_window;
        m_shared->windowReady = true;
        m_shared->started.notify_all();
    }

    static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_DEVICECHANGE) {
            if (auto* engine = reinterpret_cast<Engine*>(GetWindowLongPtrW(window, GWLP_USERDATA))) {
                return engine->deviceChange(wParam, lParam);
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static void CALLBACK completed(DWORD error, DWORD bytes, LPOVERLAPPED overlapped) {
        auto* root = static_cast<Root*>(overlapped->hEvent);
        root->ioPending = false;
        if (!root->retired) {
            root->engine->onCompletion(*root, error, bytes);
        }
    }

    [[nodiscard]] Root* rootOf(const DEV_BROADCAST_HANDLE& broadcast) {
        for (const auto& root : m_roots) {
            if ((broadcast.dbch_hdevnotify != nullptr && broadcast.dbch_hdevnotify == root->notify) ||
                (broadcast.dbch_handle != nullptr && broadcast.dbch_handle == root->lastHandle)) {
                return root.get();
            }
        }
        return nullptr;
    }

    void replaceRoots(std::vector<core::WatchRoot> roots, std::uint64_t generation) {
        for (auto& root : m_roots) {
            closeRoot(*root, true);
            retire(std::move(root));
        }
        m_roots.clear();
        m_generation = generation;
        for (std::size_t index = 0; index < roots.size(); ++index) {
            auto root = std::make_unique<Root>();
            root->engine = this;
            root->index = index;
            root->spec = std::move(roots[index]);
            root->overlapped.hEvent = root.get();
            m_roots.push_back(std::move(root));
        }
        {
            const std::lock_guard lock(m_shared->mutex);
            m_shared->status.assign(m_roots.size(), core::RootStatus{});
            m_shared->handles.assign(m_roots.size(), nullptr);
            for (const auto& root : m_roots) {
                m_shared->status[root->index].name = root->spec.name;
            }
        }
        for (const auto& root : m_roots) {
            arm(*root);
        }
        const std::lock_guard lock(m_shared->mutex);
        m_shared->settledGeneration = generation;
    }

    /// Opens the root's folder and issues its first read, or polls it, or finds it lost.
    void arm(Root& root) {
        if (root.ioPending) {
            root.nextCheck = Clock::now() + kRetrySoon; // its cancelled read has not completed yet
            return;
        }
        // Lost after its first look (gone, or not there yet): its return is a change, as a drive plugged in later.
        const bool returning = root.mode == core::WatchMode::Lost;
        if (!isFolder(root.spec.folder)) {
            lose(root, "missing", false);
            return;
        }
        if (m_options.refuseNotifications && m_options.refuseNotifications(root.spec.folder)) {
            startPolling(root, "notifications refused (test)", returning);
            return;
        }
        root.handle = CreateFileW(core::extendedLengthPath(root.spec.folder).c_str(), FILE_LIST_DIRECTORY,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (root.handle == INVALID_HANDLE_VALUE) {
            lose(root, lastErrorMessage(), false);
            return;
        }
        root.lastHandle = root.handle;
        if (!issue(root)) {
            const DWORD error = GetLastError();
            closeRoot(root, true);
            if (refusesNotifications(error)) {
                startPolling(root, errorMessage(error), returning);
            } else {
                lose(root, errorMessage(error), false);
            }
            return;
        }
        root.finalPath = finalPathOf(root.handle);
        if (m_window != nullptr && root.notify == nullptr) {
            DEV_BROADCAST_HANDLE filter{};
            filter.dbch_size = sizeof(filter);
            filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
            filter.dbch_handle = root.handle;
            root.notify = RegisterDeviceNotificationW(m_window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
            if (root.notify == nullptr) {
                spdlog::warn("Library watcher: {}: no device notifications ({}): its drive cannot be ejected while "
                             "Retroverse runs",
                             root.spec.name, lastErrorMessage());
            }
        }
        root.mode = core::WatchMode::Notify;
        root.reason.clear();
        root.nextCheck = Clock::now() + m_options.existenceInterval;
        if (returning) {
            back(root);
        }
        publish(root);
        spdlog::debug("Library watcher: {} notified", root.spec.name);
    }

    [[nodiscard]] bool issue(Root& root) {
        root.overlapped.Internal = 0;
        root.overlapped.InternalHigh = 0;
        root.overlapped.Offset = 0;
        root.overlapped.OffsetHigh = 0;
        if (!ReadDirectoryChangesW(root.handle, root.buffer.data(), kBufferBytes, root.spec.subtree ? TRUE : FALSE,
                                   kNotifyFilter, nullptr, &root.overlapped, &Engine::completed)) {
            return false;
        }
        root.ioPending = true;
        return true;
    }

    void onCompletion(Root& root, DWORD error, DWORD bytes) {
        if (root.handle == INVALID_HANDLE_VALUE || error == ERROR_OPERATION_ABORTED) {
            return; // closed on purpose
        }
        if (error == ERROR_NOTIFY_ENUM_DIR || (error == ERROR_SUCCESS && bytes == 0)) {
            push(root, core::ChangeKind::Overflow, root.spec.folder);
        } else if (error != ERROR_SUCCESS) {
            closeRoot(root, true);
            lose(root, errorMessage(error), true);
            return;
        } else {
            parse(root, bytes);
        }
        if (!isFolder(root.spec.folder)) {
            closeRoot(root, true);
            lose(root, "gone", true);
            return;
        }
        if (!issue(root)) {
            const std::string reason = lastErrorMessage();
            closeRoot(root, true);
            lose(root, reason, true);
        }
    }

    void parse(Root& root, DWORD bytes) {
        const auto* base = reinterpret_cast<const std::uint8_t*>(root.buffer.data());
        DWORD offset = 0;
        while (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) <= bytes) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
            const DWORD nameBytes = info->FileNameLength;
            if (offset + offsetof(FILE_NOTIFY_INFORMATION, FileName) + nameBytes > bytes) {
                push(root, core::ChangeKind::Overflow, root.spec.folder); // malformed: say anything may differ
                return;
            }
            const std::wstring name(info->FileName, nameBytes / sizeof(wchar_t));
            core::ChangeKind kind = core::ChangeKind::Modified;
            switch (info->Action) {
            case FILE_ACTION_ADDED:
                kind = core::ChangeKind::Added;
                break;
            case FILE_ACTION_REMOVED:
                kind = core::ChangeKind::Removed;
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                kind = core::ChangeKind::RenamedFrom;
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                kind = core::ChangeKind::RenamedTo;
                break;
            default:
                kind = core::ChangeKind::Modified;
                break;
            }
            push(root, kind, root.spec.folder / std::filesystem::path(name));
            if (info->NextEntryOffset == 0) {
                return;
            }
            offset += info->NextEntryOffset;
        }
    }

    /// Lets go of the root's handle (its read cancelled: the completion comes at the next alertable wait);
    /// `unregisterNotify` drops its device notification too.
    void closeRoot(Root& root, bool unregisterNotify) {
        if (root.handle != INVALID_HANDLE_VALUE) {
            CancelIoEx(root.handle, &root.overlapped);
            CloseHandle(root.handle);
            root.handle = INVALID_HANDLE_VALUE;
        }
        if (unregisterNotify) {
            unregister(root);
        }
        publish(root);
    }

    void unregister(Root& root) {
        if (root.notify != nullptr) {
            UnregisterDeviceNotification(root.notify);
            root.notify = nullptr;
        }
    }

    void lose(Root& root, std::string reason, bool change) {
        const bool wasWatched = root.mode == core::WatchMode::Notify || root.mode == core::WatchMode::Poll;
        root.mode = core::WatchMode::Lost;
        root.reason = std::move(reason);
        root.nextCheck = Clock::now() + m_options.existenceInterval;
        if (change && wasWatched) {
            ++root.lost;
            push(root, core::ChangeKind::RootLost, root.spec.folder);
            spdlog::info("Library watcher: {} lost ({}); its existence looked at every {} ms", root.spec.name,
                         root.reason, m_options.existenceInterval.count());
        }
        publish(root);
    }

    void back(Root& root) {
        ++root.back;
        push(root, core::ChangeKind::RootBack, root.spec.folder);
        spdlog::info("Library watcher: {} is back", root.spec.name);
    }

    void startPolling(Root& root, std::string reason, bool returning) {
        root.mode = core::WatchMode::Poll;
        root.reason = std::move(reason);
        root.snapshot.clear();
        for (const std::filesystem::path& target : root.spec.pollTargets) {
            root.snapshot[target] = entryFacts(target);
        }
        root.nextCheck = Clock::now() + m_options.pollInterval;
        if (returning) {
            back(root);
        }
        publish(root);
        spdlog::info("Library watcher: {} polled every {} ms ({})", root.spec.name, m_options.pollInterval.count(),
                     root.reason);
    }

    void poll(Root& root) {
        if (!isFolder(root.spec.folder)) {
            lose(root, "gone", true);
            return;
        }
        for (auto& [target, facts] : root.snapshot) {
            std::optional<EntryFacts> now = entryFacts(target);
            if (now != facts) {
                facts = now;
                push(root, core::ChangeKind::Polled, target);
            }
        }
        root.nextCheck = Clock::now() + m_options.pollInterval;
    }

    /// The roots whose time came: a lost root's existence, a polled root's entries, a notified root's final path.
    void tick(Clock::time_point now) {
        for (const auto& owned : m_roots) {
            Root& root = *owned;
            if (now < root.nextCheck) {
                continue;
            }
            switch (root.mode) {
            case core::WatchMode::Arming:
                arm(root);
                break;
            case core::WatchMode::Lost:
                if (root.removalPending) {
                    if (isFolder(root.spec.folder) && now - root.removalSince < m_options.removalPatience) {
                        root.nextCheck = now + m_options.existenceInterval;
                        break;
                    }
                    root.removalPending = false;
                    unregister(root);
                }
                arm(root);
                break;
            case core::WatchMode::Poll:
                poll(root);
                break;
            case core::WatchMode::Notify:
                if (root.handle != INVALID_HANDLE_VALUE && finalPathOf(root.handle) != root.finalPath) {
                    closeRoot(root, true);
                    lose(root, "moved", true);
                } else {
                    root.nextCheck = now + m_options.existenceInterval;
                }
                break;
            }
        }
        // Roots retired with a read still pending go once it completed.
        std::erase_if(m_retired, [](const std::unique_ptr<Root>& root) { return !root->ioPending; });
    }

    [[nodiscard]] DWORD timeoutUntilNext(Clock::time_point now) const {
        std::optional<Clock::time_point> next;
        for (const auto& root : m_roots) {
            if (!next || root->nextCheck < *next) {
                next = root->nextCheck;
            }
        }
        if (!m_retired.empty()) {
            const Clock::time_point soon = now + kRetrySoon;
            next = next ? std::min(*next, soon) : soon;
        }
        if (!next) {
            return INFINITE;
        }
        if (*next <= now) {
            return 0;
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(*next - now).count() + 1;
        return static_cast<DWORD>(std::min<long long>(wait, 24LL * 3600 * 1000));
    }

    void retire(std::unique_ptr<Root> root) {
        root->retired = true;
        unregister(*root);
        if (root->ioPending) {
            m_retired.push_back(std::move(root));
        }
    }

    /// Waits (alertably) for the completions of the reads cancelled at the end, within kDrainBound.
    void drain() {
        const Clock::time_point until = Clock::now() + kDrainBound;
        while (!m_retired.empty() && Clock::now() < until) {
            SleepEx(10, TRUE);
            std::erase_if(m_retired, [](const std::unique_ptr<Root>& root) { return !root->ioPending; });
        }
        if (!m_retired.empty()) {
            // Their buffers stay the kernel's: leaked rather than freed under a pending read.
            spdlog::warn("Library watcher: {} cancelled read(s) did not complete; their buffers are left", m_retired.size());
            for (auto& root : m_retired) {
                static_cast<void>(root.release());
            }
            m_retired.clear();
        }
    }

    void push(const Root& root, core::ChangeKind kind, std::filesystem::path path) {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->push(core::Change{root.index, kind, std::move(path)});
        m_pushed = true;
    }

    void publish(const Root& root) {
        const std::lock_guard lock(m_shared->mutex);
        if (root.retired || root.index >= m_shared->status.size()) {
            return;
        }
        core::RootStatus& status = m_shared->status[root.index];
        status.mode = root.mode;
        status.lost = root.lost;
        status.back = root.back;
        status.reason = root.reason;
        m_shared->handles[root.index] = root.handle == INVALID_HANDLE_VALUE ? nullptr : root.handle;
    }

    /// Wakes the host after changes were queued.
    void flush() {
        if (!std::exchange(m_pushed, false)) {
            return;
        }
        if (m_options.wake) {
            m_options.wake();
        }
    }

    std::shared_ptr<DirectoryWatcher::Shared> m_shared;
    DirectoryWatcher::Options m_options;
    std::uint64_t m_generation = 0;
    std::vector<std::unique_ptr<Root>> m_roots;
    std::vector<std::unique_ptr<Root>> m_retired;
    HWND m_window = nullptr;
    bool m_pushed = false;
};

} // namespace

DirectoryWatcher::DirectoryWatcher(Options options) : m_shared(std::make_shared<Shared>()) {
    m_shared->options = std::move(options);
    m_shared->command = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_thread = std::thread([shared = m_shared] {
        Engine engine(shared);
        engine.run();
    });
    std::unique_lock lock(m_shared->mutex);
    m_shared->started.wait(lock, [&] { return m_shared->windowReady; });
}

DirectoryWatcher::~DirectoryWatcher() {
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->stopping = true;
    }
    SetEvent(m_shared->command);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    CloseHandle(m_shared->command);
}

void DirectoryWatcher::watch(std::vector<core::WatchRoot> roots) {
    {
        const std::lock_guard lock(m_shared->mutex);
        m_shared->status.assign(roots.size(), core::RootStatus{});
        for (std::size_t index = 0; index < roots.size(); ++index) {
            m_shared->status[index].name = roots[index].name;
        }
        m_shared->handles.assign(roots.size(), nullptr);
        m_shared->roots = std::move(roots);
        ++m_shared->generation;
        m_shared->changes.clear();
        m_shared->overflowed.clear();
    }
    SetEvent(m_shared->command);
}

std::vector<core::Change> DirectoryWatcher::take() {
    const std::lock_guard lock(m_shared->mutex);
    std::vector<core::Change> changes = std::move(m_shared->changes);
    m_shared->changes.clear();
    for (const std::size_t root : m_shared->overflowed) {
        const std::filesystem::path folder =
            root < m_shared->roots.size() ? m_shared->roots[root].folder : std::filesystem::path();
        changes.push_back(core::Change{root, core::ChangeKind::Overflow, folder});
    }
    m_shared->overflowed.clear();
    return changes;
}

std::vector<core::RootStatus> DirectoryWatcher::status() const {
    const std::lock_guard lock(m_shared->mutex);
    return m_shared->status;
}

bool DirectoryWatcher::settled() const {
    const std::lock_guard lock(m_shared->mutex);
    return m_shared->settledGeneration == m_shared->generation;
}

void* DirectoryWatcher::notificationWindow() const {
    const std::lock_guard lock(m_shared->mutex);
    return m_shared->window;
}

void* DirectoryWatcher::rootHandle(std::size_t root) const {
    const std::lock_guard lock(m_shared->mutex);
    return root < m_shared->handles.size() ? m_shared->handles[root] : nullptr;
}

} // namespace rv::platform_win
