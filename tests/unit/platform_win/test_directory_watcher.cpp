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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    // While the removal is pending, nothing is told (no refresh may open the volume Windows removes), and the folder
    // still there is left alone (the existence poll arms nothing).
    std::this_thread::sleep_for(500ms);
    CHECK(watcher.take().empty());
    CHECK(watcher.status()[0].lost == 0);
    CHECK(modeOf(watcher) == WatchMode::Lost);
    CHECK(openExclusively(scratch.root));

    // Another program vetoed the removal: watched again, and nothing changed.
    CHECK(SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEQUERYREMOVEFAILED, reinterpret_cast<LPARAM>(&broadcast)) ==
          TRUE);
    CHECK(waitFor([&] { return modeOf(watcher) == WatchMode::Notify; }));
    CHECK_FALSE(openExclusively(scratch.root));
    std::this_thread::sleep_for(300ms);
    CHECK(watcher.take().empty());
    CHECK(watcher.status()[0].back == 0);
    // A notification about another handle changes nothing.
    DEV_BROADCAST_HANDLE other = broadcast;
    other.dbch_handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(0x7FFF0));
    CHECK(SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEQUERYREMOVE, reinterpret_cast<LPARAM>(&other)) == TRUE);
    CHECK(modeOf(watcher) == WatchMode::Notify);

    // Asked again, and the volume goes this time (its folder no longer there): the loss is told then, once.
    broadcast.dbch_handle = static_cast<HANDLE>(watcher.rootHandle(0));
    CHECK(SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEQUERYREMOVE, reinterpret_cast<LPARAM>(&broadcast)) == TRUE);
    CHECK(openExclusively(scratch.root));
    const fs::path away = scratch.fixture.root() / "watched-ejected";
    fs::rename(scratch.root, away);
    std::vector<Change> changes;
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RootLost, scratch.root)));
    CHECK(watcher.status()[0].lost == 1);
    fs::rename(away, scratch.root); // plugged in again
    CHECK(waitForChange(watcher, changes, is(ChangeKind::RootBack, scratch.root)));
    CHECK(waitFor([&] { return modeOf(watcher) == WatchMode::Notify; }));

    // A surprise removal: closed and told at once, then armed again once the folder is seen there.
    broadcast.dbch_handle = static_cast<HANDLE>(watcher.rootHandle(0));
    CHECK(SendMessageW(window, WM_DEVICECHANGE, DBT_DEVICEREMOVECOMPLETE, reinterpret_cast<LPARAM>(&broadcast)) == TRUE);
    CHECK(waitFor([&] { return watcher.status()[0].lost == 2; }));
    CHECK(waitFor([&] { return modeOf(watcher) == WatchMode::Notify; }));
}

TEST_CASE("platform_win/directory_watcher: a folder renamed in, a dotted name too, is marked a folder",
          "[platform_win][directory_watcher]") {
    const Scratch scratch;
    const fs::path outside = scratch.fixture.root() / "outside";
    fs::create_directories(outside / "Collection v2.0");
    write(outside / "Collection v2.0" / "Inner (USA).sfc", "rom");
    write(outside / "Game (Rev 1.1).sfc", "rom");
    DirectoryWatcher watcher(fastOptions());
    watcher.watch({rootOf(scratch.root)});
    REQUIRE(waitFor([&] { return watcher.settled() && modeOf(watcher) == WatchMode::Notify; }));
    std::vector<Change> changes;
    const fs::path folder = scratch.root / "snes" / "Collection v2.0";
    const fs::path file = scratch.root / "snes" / "Game (Rev 1.1).sfc";
    fs::rename(outside / "Collection v2.0", folder); // one volume: the folder's event alone
    fs::rename(outside / "Game (Rev 1.1).sfc", file);
    CHECK(waitForChange(watcher, changes, [&](const Change& change) {
        return change.directory && fs::path(change.path).lexically_normal() == folder.lexically_normal();
    }));
    CHECK(waitForChange(watcher, changes, [&](const Change& change) {
        return fs::path(change.path).lexically_normal() == file.lexically_normal();
    }));
    for (const Change& change : changes) {
        if (fs::path(change.path).lexically_normal() == file.lexically_normal()) {
            CHECK_FALSE(change.directory);
        }
        CHECK(fs::path(change.path).lexically_normal() != (folder / "Inner (USA).sfc").lexically_normal());
    }
}

namespace {

/// A FILE_NOTIFY_INFORMATION buffer: each entry DWORD aligned, the last with NextEntryOffset 0.
std::vector<std::byte> notifyBuffer(const std::vector<std::pair<DWORD, std::wstring>>& entries) {
    std::vector<std::byte> buffer;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::size_t start = buffer.size();
        const std::size_t nameBytes = entries[index].second.size() * sizeof(wchar_t);
        std::size_t size = offsetof(FILE_NOTIFY_INFORMATION, FileName) + nameBytes;
        size = (size + 3) / 4 * 4;
        buffer.resize(start + size);
        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data() + start);
        info->NextEntryOffset = index + 1 == entries.size() ? 0 : static_cast<DWORD>(size);
        info->Action = entries[index].first;
        info->FileNameLength = static_cast<DWORD>(nameBytes);
        std::memcpy(info->FileName, entries[index].second.data(), nameBytes);
    }
    return buffer;
}

} // namespace

TEST_CASE("platform_win/directory_watcher: a read's buffer: changes, overflows, a malformed buffer, an error",
          "[platform_win][directory_watcher]") {
    using rv::platform_win::detail::readNotifications;
    const fs::path folder = "X:\\Watched";
    const std::vector<std::byte> buffer = notifyBuffer({{FILE_ACTION_ADDED, L"snes\\A.sfc"},
                                                        {FILE_ACTION_RENAMED_OLD_NAME, L"B.sfc"},
                                                        {FILE_ACTION_RENAMED_NEW_NAME, L"C.sfc"},
                                                        {FILE_ACTION_MODIFIED, L"D.sfc"},
                                                        {FILE_ACTION_REMOVED, L"E.sfc"}});
    const auto read = readNotifications(buffer, ERROR_SUCCESS, static_cast<std::uint32_t>(buffer.size()), folder);
    CHECK_FALSE(read.overflow);
    CHECK_FALSE(read.failed);
    REQUIRE(read.changes.size() == 5);
    CHECK(read.changes[0].first == ChangeKind::Added);
    CHECK(read.changes[0].second == folder / L"snes\\A.sfc");
    CHECK(read.changes[1].first == ChangeKind::RenamedFrom);
    CHECK(read.changes[2].first == ChangeKind::RenamedTo);
    CHECK(read.changes[3].first == ChangeKind::Modified);
    CHECK(read.changes[4].first == ChangeKind::Removed);
    CHECK(read.changes[4].second == folder / L"E.sfc");

    // The file system's buffer overflowed: ERROR_NOTIFY_ENUM_DIR, or a success of 0 bytes.
    const auto enumDir = readNotifications(buffer, ERROR_NOTIFY_ENUM_DIR, 0, folder);
    CHECK(enumDir.overflow);
    CHECK(enumDir.changes.empty());
    const auto empty = readNotifications(buffer, ERROR_SUCCESS, 0, folder);
    CHECK(empty.overflow);
    CHECK_FALSE(empty.failed);
    // A buffer cut short (its last name runs past the bytes): the changes before it kept, an overflow.
    const auto cut = readNotifications(buffer, ERROR_SUCCESS, static_cast<std::uint32_t>(buffer.size() - 6), folder);
    CHECK(cut.overflow);
    CHECK(cut.changes.size() == 4);
    // An offset that points past the buffer; an odd name length.
    std::vector<std::byte> bad = notifyBuffer({{FILE_ACTION_ADDED, L"A.sfc"}, {FILE_ACTION_ADDED, L"B.sfc"}});
    reinterpret_cast<FILE_NOTIFY_INFORMATION*>(bad.data())->NextEntryOffset = 4096;
    const auto pastEnd = readNotifications(bad, ERROR_SUCCESS, static_cast<std::uint32_t>(bad.size()), folder);
    CHECK(pastEnd.overflow);
    CHECK(pastEnd.changes.size() == 1);
    std::vector<std::byte> odd = notifyBuffer({{FILE_ACTION_ADDED, L"A.sfc"}});
    reinterpret_cast<FILE_NOTIFY_INFORMATION*>(odd.data())->FileNameLength = 3;
    CHECK(readNotifications(odd, ERROR_SUCCESS, static_cast<std::uint32_t>(odd.size()), folder).overflow);
    // Any other error: the root failed.
    const auto denied = readNotifications(buffer, ERROR_ACCESS_DENIED, 0, folder);
    CHECK(denied.failed);
    CHECK_FALSE(denied.overflow);
}

TEST_CASE("platform_win/directory_watcher: past kMaxPendingChanges a root's changes are one Overflow at take()",
          "[platform_win][directory_watcher]") {
    rv::platform_win::detail::PendingChanges pending;
    const std::vector<rv::core::WatchRoot> roots{rootOf("X:\\Roms"), rv::core::WatchRoot{"media", "X:\\Media", true, {}}};
    for (std::size_t index = 0; index < rv::core::kMaxPendingChanges; ++index) {
        pending.push(Change{0, ChangeKind::Added, fs::path("X:\\Roms") / std::to_string(index)});
    }
    CHECK(pending.size() == rv::core::kMaxPendingChanges);
    pending.push(Change{0, ChangeKind::Added, "X:\\Roms\\late-1"});
    pending.push(Change{0, ChangeKind::Added, "X:\\Roms\\late-2"});
    pending.push(Change{1, ChangeKind::Added, "X:\\Media\\late"});
    CHECK(pending.size() == rv::core::kMaxPendingChanges);
    const std::vector<Change> taken = pending.take(roots);
    REQUIRE(taken.size() == rv::core::kMaxPendingChanges + 2);
    CHECK(taken[rv::core::kMaxPendingChanges] == Change{0, ChangeKind::Overflow, "X:\\Roms"});
    CHECK(taken[rv::core::kMaxPendingChanges + 1] == Change{1, ChangeKind::Overflow, "X:\\Media"});
    // Emptied: the next take() has nothing, and room again.
    CHECK(pending.take(roots).empty());
    pending.push(Change{0, ChangeKind::Removed, "X:\\Roms\\again"});
    CHECK(pending.take(roots).size() == 1);
}

TEST_CASE("platform_win/directory_watcher: the watcher's names", "[platform_win][directory_watcher]") {
    CHECK(rv::core::toString(WatchMode::Notify) == "notify");
    CHECK(rv::core::toString(WatchMode::Poll) == "poll");
    CHECK(rv::core::toString(WatchMode::Lost) == "lost");
    CHECK(rv::core::toString(ChangeKind::RenamedFrom) == "renamed-from");
    CHECK(rv::core::toString(ChangeKind::RootBack) == "root-back");
}
