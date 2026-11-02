#pragma once

// The Windows adapter of core::IChangeWatcher (ADR-0018 §10): one thread, below normal priority, that watches every
// root with an overlapped ReadDirectoryChangesW (a completion routine per root, so that any number of roots shares one
// alertable wait), read-only: each folder is opened with FILE_LIST_DIRECTORY alone and full sharing, nothing is ever
// written. Besides the notifications:
//  - a root that is not there, or whose handle fails, is closed and its existence looked at every existenceInterval
//    (core::kWatchExistenceInterval, 5 s); when it is back it is armed again (RootLost, RootBack). A notified root's
//    handle is asked its final path on the same interval (no disk read): a root renamed away is lost too;
//  - safe removal: the thread owns a message-only window and registers each root's handle for device notifications
//    (RegisterDeviceNotificationW, DBT_DEVTYP_HANDLE). DBT_DEVICEQUERYREMOVE closes the handle at once, so the volume can
//    be ejected; DBT_DEVICEQUERYREMOVEFAILED arms it again; DBT_DEVICEREMOVECOMPLETE leaves it to the existence poll. A
//    removal asked and never answered arms the root again only after removalPatience with its folder still there;
//  - the fallback: a root where ReadDirectoryChangesW is refused (ERROR_INVALID_FUNCTION, ERROR_NOT_SUPPORTED,
//    ERROR_INVALID_PARAMETER: a file system or a share without notifications) is polled every pollInterval
//    (core::kWatchPollInterval, 30 s): its WatchRoot::pollTargets' size and last write time, never a walk. It never
//    runs on a root whose notifications work, since it wakes the disk.
// The thread calls nothing of SDL: Options::wake (the host's) after it queued changes.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <rv/core/change_watcher.hpp>

namespace rv::platform_win {

class DirectoryWatcher final : public core::IChangeWatcher {
public:
    struct Options {
        /// Called on the watcher's thread after it queued changes (AppContext::wakeCallback(): SDL_PushEvent). It must be
        /// safe to call from another thread.
        std::function<void()> wake;
        std::chrono::milliseconds existenceInterval = core::kWatchExistenceInterval;
        std::chrono::milliseconds pollInterval = core::kWatchPollInterval;
        /// How long a volume asked to go (DBT_DEVICEQUERYREMOVE) is left alone while its folder stays and no answer comes.
        std::chrono::milliseconds removalPatience{60000};
        /// Tests: a folder this answers true for refuses notifications, as a file system without them refuses them.
        std::function<bool(const std::filesystem::path&)> refuseNotifications;
    };

    explicit DirectoryWatcher(Options options = {});
    /// Closes every handle and ends the thread. A thread held in a synchronous call (a share that stopped answering) has
    /// that call cancelled (CancelSynchronousIo); one still not ended after kStopBound is left to end by itself.
    ~DirectoryWatcher() override;
    static constexpr std::chrono::milliseconds kStopBound{5000};
    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    void watch(std::vector<core::WatchRoot> roots) override;
    [[nodiscard]] std::vector<core::Change> take() override;
    [[nodiscard]] std::vector<core::RootStatus> status() const override;

    /// Every root of the last watch() was looked at once (armed, polled or found lost).
    [[nodiscard]] bool settled() const;
    /// Tests: the message-only window that receives the device notifications (an HWND), and the directory handle `root`
    /// is notified through now (a HANDLE; null when it holds none).
    [[nodiscard]] void* notificationWindow() const;
    [[nodiscard]] void* rootHandle(std::size_t root) const;

    /// What the thread and the callers share (directory_watcher.cpp).
    struct Shared;

private:
    std::shared_ptr<Shared> m_shared;
    std::thread m_thread;
};

namespace detail {

/// What one completed ReadDirectoryChangesW brought (DirectoryWatcher's thread, apart for its tests).
struct ReadOutcome {
    /// The changes of the buffer, in its order: their kind and absolute path (`folder` / the name read).
    std::vector<std::pair<core::ChangeKind, std::filesystem::path>> changes;
    /// Changes were lost: ERROR_NOTIFY_ENUM_DIR, a success of 0 bytes (the file system's buffer overflowed), a buffer
    /// that does not hold what its offsets say (the changes read before the bad entry are kept).
    bool overflow = false;
    /// Any other error: the root is lost.
    bool failed = false;
};

/// Reads `bytes` of `buffer` (FILE_NOTIFY_INFORMATION entries, DWORD aligned) after a read that ended with `error`.
[[nodiscard]] ReadOutcome readNotifications(std::span<const std::byte> buffer, std::uint32_t error, std::uint32_t bytes,
                                           const std::filesystem::path& folder);

/// The changes waiting for take(): at most core::kMaxPendingChanges; past that a root's further changes are one
/// Overflow of that root, which take() adds after the changes kept.
class PendingChanges {
public:
    void push(core::Change change);
    /// The changes, then an Overflow per root that lost some (its folder from `roots`, by index); empties the queue.
    [[nodiscard]] std::vector<core::Change> take(const std::vector<core::WatchRoot>& roots);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return m_changes.size(); }

private:
    std::vector<core::Change> m_changes;
    std::vector<std::size_t> m_overflowed;
};

} // namespace detail

} // namespace rv::platform_win
