#pragma once

// The change-watcher port (ADR-0018 §10): folders watched for what is added, removed, renamed or written below them,
// read-only. The application names the roots (the ROM root, each system folder outside it, the media root, the ES-DE
// home's gamelists, custom_systems and settings folders) and takes the changes on its own thread; what a change means
// (which system, if any) is decided above the port (esde::changeScope()). platform_win::DirectoryWatcher is the
// Windows adapter (ReadDirectoryChangesW on one thread, the existence poll of a root that went, the fallback poll of a
// root whose file system refuses notifications); tests use a fake (tests/support/fake_change_watcher.hpp).
//
// Nothing here writes: an adapter opens folders for listing only, with full sharing, and closes a folder's handle as
// soon as its volume is asked to go.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rv::core {

/// A folder to watch.
struct WatchRoot {
    /// What it is, for people and the run report ("rom", "system:snes", "media", "gamelists", "custom-systems",
    /// "settings"): never a path, which may name the user's folders.
    std::string name;
    std::filesystem::path folder;
    /// The folders below it too (a ROM root, a media root), or only its own entries (the settings folder).
    bool subtree = true;
    /// The fallback poll (a root whose file system refuses notifications): these files and folders are looked at, their
    /// size and last write time, never a walk below them; a difference is a change of that path (ChangeKind::Polled).
    std::vector<std::filesystem::path> pollTargets;

    bool operator==(const WatchRoot&) const = default;
};

/// How a root is watched now.
enum class WatchMode : std::uint8_t {
    Arming, ///< asked, not looked at yet
    Notify, ///< the file system's notifications (ReadDirectoryChangesW)
    Poll,   ///< notifications refused: WatchRoot::pollTargets looked at on an interval
    Lost,   ///< the folder is not there, or its volume went: its existence looked at on an interval
};

/// "arming", "notify", "poll", "lost".
[[nodiscard]] std::string_view toString(WatchMode mode) noexcept;

/// What happened.
enum class ChangeKind : std::uint8_t {
    Added,
    Removed,
    Modified,
    RenamedFrom,
    RenamedTo,
    Polled,   ///< the fallback poll saw a WatchRoot::pollTargets entry change
    Overflow, ///< changes were lost (the file system's buffer, or too many waiting): anything below the root may differ
    RootLost, ///< the root went (deleted, renamed, its volume removed or asked to go): watched again once it is back
    RootBack, ///< the root is there again and watched
};

/// "added", "removed", "modified", "renamed-from", "renamed-to", "polled", "overflow", "root-lost", "root-back".
[[nodiscard]] std::string_view toString(ChangeKind kind) noexcept;

/// One change.
struct Change {
    /// The index of its root in the last IChangeWatcher::watch() list.
    std::size_t root = 0;
    ChangeKind kind = ChangeKind::Modified;
    /// The absolute path that changed (the root's folder for Overflow, RootLost and RootBack).
    std::filesystem::path path;
    /// Added and RenamedTo: the path was a folder when the watcher looked (a folder moved in, whose own files a move on
    /// one volume never reports).
    bool directory = false;

    bool operator==(const Change&) const = default;
};

/// Where a root stands.
struct RootStatus {
    std::string name;
    WatchMode mode = WatchMode::Arming;
    /// Times it was lost, and came back.
    std::uint64_t lost = 0;
    std::uint64_t back = 0;
    /// Why it is not notified (the error that refused notifications, or what made it lost); empty when notified.
    std::string reason;

    bool operator==(const RootStatus&) const = default;
};

/// The most changes kept between two IChangeWatcher::take() calls; past it a root's further changes are one Overflow.
inline constexpr std::size_t kMaxPendingChanges = 4096;
/// How often a lost root's existence is looked at (ADR-0018 §10).
inline constexpr std::chrono::milliseconds kWatchExistenceInterval{5000};
/// How often a root whose notifications are refused is polled (ADR-0018 §10).
inline constexpr std::chrono::milliseconds kWatchPollInterval{30000};

/// The port. Thread-safe: watch() and take() from the application's thread, the changes made on the adapter's own.
class IChangeWatcher {
public:
    virtual ~IChangeWatcher() = default;
    /// Watches exactly `roots` from now on (an empty list stops watching); returns at once: the roots are armed on the
    /// adapter's thread. Changes not taken yet are dropped.
    virtual void watch(std::vector<WatchRoot> roots) = 0;
    /// The changes since the last call, oldest first.
    [[nodiscard]] virtual std::vector<Change> take() = 0;
    /// Each root of the last watch() list, in its order.
    [[nodiscard]] virtual std::vector<RootStatus> status() const = 0;
};

} // namespace rv::core
