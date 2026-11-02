// rv_platform_win's DirectoryWatcher (I7 step 7, ADR-0018 §10) on real folders under the build tree (a materialized
// fixture: never the user's): ReadDirectoryChangesW's notifications (added, modified, renamed, removed) on the watcher's
// thread with the host woken; a root renamed away and back, deleted and made again, missing at first (lost, its
// existence looked at, armed again); the fallback poll of a root whose notifications are refused (its entries' size and
// time, nothing below them); a device notification (DBT_DEVICEQUERYREMOVE sent to the watcher's message-only window, as
// Windows sends it before an eject) closing the handle at once, the root left alone while the removal is pending, and
// DBT_DEVICEQUERYREMOVEFAILED arming it again; an empty list closing everything. The handles only read: the folder is
// opened exclusively by the test whenever the watcher holds nothing, and refused while it does.

#include <rv/platform_win/directory_watcher.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <rv/core/file_io.hpp>

#include "support/fixture.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After windows.h.
#include <dbt.h>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using rv::core::Change;
using rv::core::ChangeKind;
using rv::core::WatchMode;
using rv::platform_win::DirectoryWatcher;

constexpr auto kTimeout = 10s;

/// A scratch folder under the build tree (a materialized fixture's).
struct Scratch {
    rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/snes-single").value();
    fs::path root = fixture.root() / "watched";

    Scratch() { fs::create_directories(root / "snes"); }
};

/// Written in place (no temporary file renamed over it): one file added, as a copy adds it.
void write(const fs::path& file, std::string_view text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    REQUIRE(out.good());
}

/// The changes taken until `found` holds one that `match` accepts, within kTimeout.
bool waitForChange(DirectoryWatcher& watcher, std::vector<Change>& found, const std::function<bool(const Change&)>& match) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        for (Change& change : watcher.take()) {
            found.push_back(std::move(change));
        }
        if (std::any_of(found.begin(), found.end(), match)) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

bool waitFor(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

std::function<bool(const Change&)> is(ChangeKind kind, const fs::path& path) {
    return [kind, path](const Change& change) {
        return change.kind == kind && fs::path(change.path).lexically_normal() == path.lexically_normal();
    };
}

WatchMode modeOf(const DirectoryWatcher& watcher, std::size_t root = 0) {
    const auto status = watcher.status();
    return root < status.size() ? status[root].mode : WatchMode::Arming;
}

/// Whether the folder can be opened with no sharing at all: only when no other handle holds it.
bool openExclusively(const fs::path& folder) {
    const HANDLE handle = CreateFileW(rv::core::extendedLengthPath(folder).c_str(), FILE_LIST_DIRECTORY, 0, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(handle);
    return true;
}

DirectoryWatcher::Options fastOptions(std::atomic<int>* wakes = nullptr) {
    DirectoryWatcher::Options options;
    options.existenceInterval = 100ms;
    options.pollInterval = 200ms;
    if (wakes != nullptr) {
        options.wake = [wakes] { ++*wakes; };
    }
    return options;
}

rv::core::WatchRoot rootOf(const fs::path& folder, std::vector<fs::path> targets = {}) {
    return rv::core::WatchRoot{"rom", folder, true, std::move(targets)};
}

} // namespace

TEST_CASE("platform_win/directory_watcher: files added, written, renamed and removed are notified; the host is woken",
          "[platform_win][directory_watcher]") {
    const Scratch scratch;
    std::atomic<int> wakes{0};
    DirectoryWatcher watcher(fastOptions(&wakes));
    watcher.watch({rootOf(scratch.root)});
    REQUIRE(waitFor([&] { return watcher.settled(); }));
    REQUIRE(modeOf(watcher) == WatchMode::Notify);
    CHECK(watcher.status()[0].name == "rom");
    CHECK(watcher.rootHandle(0) != nullptr);
    CHECK_FALSE(openExclusively(scratch.root)); // held, for reading only

    std::vector<Change> changes;
    const fs::path rom = scratch.root / "snes" / "Zinnia (USA).sfc";
    write(rom, "rom");
    CHECK(waitForChange(watcher, changes, is(ChangeKind::Added, rom)));
    CHECK(wakes.load() > 0);
    // The watcher never stands in the way of a file being renamed, written or removed.
    const fs::path renamed = scratch.root / "snes" / "Zinnia (Europe).sfc";
    fs::rename(rom, renamed);
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RenamedFrom, rom)));
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RenamedTo, renamed)));
    {
        std::FILE* file = _wfopen(renamed.c_str(), L"ab");
        REQUIRE(file != nullptr);
        std::fputs("more", file);
        std::fclose(file);
    }
    CHECK(waitForChange(watcher, changes, is(ChangeKind::Modified, renamed)));
    fs::remove(renamed);
    CHECK(waitForChange(watcher, changes, is(ChangeKind::Removed, renamed)));
    for (const Change& change : changes) {
        CHECK(change.root == 0);
    }

    // An empty list lets go of everything.
    watcher.watch({});
    REQUIRE(waitFor([&] { return watcher.settled(); }));
    CHECK(waitFor([&] { return openExclusively(scratch.root); }));
    CHECK(watcher.status().empty());
}

TEST_CASE("platform_win/directory_watcher: a root renamed away is lost, its existence looked at; back, it is armed again",
          "[platform_win][directory_watcher]") {
    const Scratch scratch;
    DirectoryWatcher watcher(fastOptions());
    watcher.watch({rootOf(scratch.root)});
    REQUIRE(waitFor([&] { return watcher.settled() && modeOf(watcher) == WatchMode::Notify; }));

    std::vector<Change> changes;
    const fs::path away = scratch.fixture.root() / "watched-away";
    fs::rename(scratch.root, away); // possible while watched: the handle shares deletion
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RootLost, scratch.root)));
    CHECK(modeOf(watcher) == WatchMode::Lost);
    CHECK(watcher.status()[0].lost == 1);
    CHECK(watcher.rootHandle(0) == nullptr);
    CHECK(openExclusively(away)); // nothing held on the folder that moved
    fs::rename(away, scratch.root);
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RootBack, scratch.root)));
    CHECK(waitFor([&] { return modeOf(watcher) == WatchMode::Notify; }));
    CHECK(watcher.status()[0].back == 1);
    const fs::path rom = scratch.root / "snes" / "After (USA).sfc";
    write(rom, "rom");
    CHECK(waitForChange(watcher, changes, is(ChangeKind::Added, rom)));
}

TEST_CASE("platform_win/directory_watcher: a root deleted and made again; a root missing at first",
          "[platform_win][directory_watcher]") {
    const Scratch scratch;
    const fs::path later = scratch.fixture.root() / "later";
    DirectoryWatcher watcher(fastOptions());
    watcher.watch({rootOf(scratch.root), rv::core::WatchRoot{"media", later, true, {}}});
    REQUIRE(waitFor([&] { return watcher.settled(); }));
    CHECK(modeOf(watcher, 0) == WatchMode::Notify);
    CHECK(modeOf(watcher, 1) == WatchMode::Lost); // not there yet: no change for it
    CHECK(watcher.status()[1].lost == 0);

    std::vector<Change> changes;
    fs::create_directories(later);
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RootBack, later))); // a drive plugged in after the start
    CHECK(waitFor([&] { return modeOf(watcher, 1) == WatchMode::Notify; }));

    fs::remove_all(scratch.root);
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RootLost, scratch.root)));
    CHECK(waitFor([&] { return !fs::exists(scratch.root); })); // the delete completed: no handle held it pending
    fs::create_directories(scratch.root);
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RootBack, scratch.root)));
    CHECK(waitFor([&] { return modeOf(watcher, 0) == WatchMode::Notify; }));
}

TEST_CASE("platform_win/directory_watcher: refused notifications fall back to polling the entries, never below them",
          "[platform_win][directory_watcher]") {
    const Scratch scratch;
    const fs::path gamelist = scratch.root / "gamelist.xml";
    write(gamelist, "<gameList/>");
    DirectoryWatcher::Options options = fastOptions();
    options.refuseNotifications = [](const fs::path&) { return true; }; // a file system without notifications
    DirectoryWatcher watcher(options);
    watcher.watch({rootOf(scratch.root, {scratch.root / "snes", gamelist, scratch.root / "megadrive"})});
    REQUIRE(waitFor([&] { return watcher.settled(); }));
    CHECK(modeOf(watcher) == WatchMode::Poll);
    CHECK_FALSE(watcher.status()[0].reason.empty());
    CHECK(watcher.rootHandle(0) == nullptr);
    CHECK(openExclusively(scratch.root)); // polling holds nothing open

    std::vector<Change> changes;
    write(gamelist, "<gameList><game/></gameList>"); // its size changes
    CHECK(waitForChange(watcher, changes, is(ChangeKind::Polled, gamelist)));
    fs::create_directories(scratch.root / "megadrive"); // an entry that appears
    CHECK(waitForChange(watcher, changes, is(ChangeKind::Polled, scratch.root / "megadrive")));
    // A file deep below an entry is not looked at: only the entries' own facts.
    changes.clear();
    fs::create_directories(scratch.root / "snes" / "deep");
    static_cast<void>(watcher.take());
    write(scratch.root / "snes" / "deep" / "Hidden (USA).sfc", "rom");
    std::this_thread::sleep_for(700ms); // more than three polls
    for (const Change& change : watcher.take()) {
        CHECK(change.path != scratch.root / "snes" / "deep" / "Hidden (USA).sfc");
    }
}

TEST_CASE("platform_win/directory_watcher: a volume asked to go: the handle closed at once; the removal refused: armed "
          "again",
          "[platform_win][directory_watcher]") {
    const Scratch scratch;
    DirectoryWatcher watcher(fastOptions());
    watcher.watch({rootOf(scratch.root)});
    REQUIRE(waitFor([&] { return watcher.settled() && modeOf(watcher) == WatchMode::Notify; }));
    const auto window = static_cast<HWND>(watcher.notificationWindow());
    REQUIRE(window != nullptr);
    const auto handle = static_cast<HANDLE>(watcher.rootHandle(0));
    REQUIRE(handle != nullptr);
    REQUIRE_FALSE(openExclusively(scratch.root));

    // What Windows sends a window registered for the handle before ejecting its volume.
    DEV_BROADCAST_HANDLE broadcast{};
    broadcast.dbch_size = sizeof(broadcast);
    broadcast.dbch_devicetype = DBT_DEVTYP_HANDLE;
    broadcast.dbch_handle = handle;
    CHECK(SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEQUERYREMOVE, reinterpret_cast<LPARAM>(&broadcast)) == TRUE);
    // Answered within the message: nothing holds the folder any more.
    CHECK(openExclusively(scratch.root));
    CHECK(modeOf(watcher) == WatchMode::Lost);
    CHECK(watcher.status()[0].reason == "volume-removal");
    std::vector<Change> changes;
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RootLost, scratch.root)));
    // While the removal is pending, the folder still there is left alone (the existence poll arms nothing).
    std::this_thread::sleep_for(500ms);
    CHECK(modeOf(watcher) == WatchMode::Lost);
    CHECK(openExclusively(scratch.root));

    // Another program vetoed the removal: watched again.
    CHECK(SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEQUERYREMOVEFAILED, reinterpret_cast<LPARAM>(&broadcast)) ==
          TRUE);
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RootBack, scratch.root)));
    CHECK(waitFor([&] { return modeOf(watcher) == WatchMode::Notify; }));
    CHECK_FALSE(openExclusively(scratch.root));
    // A notification about another handle changes nothing.
    DEV_BROADCAST_HANDLE other = broadcast;
    other.dbch_handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x7FFF0));
    CHECK(SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEQUERYREMOVE, reinterpret_cast<LPARAM>(&other)) == TRUE);
    CHECK(modeOf(watcher) == WatchMode::Notify);

    // The volume went for good (a surprise removal): closed, then armed again once the folder is seen there.
    broadcast.dbch_handle = static_cast<HANDLE>(watcher.rootHandle(0));
    CHECK(SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEREMOVECOMPLETE, reinterpret_cast<LPARAM>(&broadcast)) == TRUE);
    CHECK(waitFor([&] { return watcher.status()[0].lost == 2; }));
    CHECK(waitFor([&] { return modeOf(watcher) == WatchMode::Notify; }));
}

TEST_CASE("platform_win/directory_watcher: the watcher's names", "[platform_win][directory_watcher]") {
    CHECK(rv::core::toString(WatchMode::Notify) == "notify");
    CHECK(rv::core::toString(WatchMode::Poll) == "poll");
    CHECK(rv::core::toString(WatchMode::Lost) == "lost");
    CHECK(rv::core::toString(ChangeKind::RenamedFrom) == "renamed-from");
    CHECK(rv::core::toString(ChangeKind::RootBack) == "root-back");
}
