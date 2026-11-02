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
#include <memory>
#include <thread>
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
    /// Closes every handle and ends the thread.
    ~DirectoryWatcher() override;
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

} // namespace rv::platform_win
