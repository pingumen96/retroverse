// The file watcher's policy (I7 step 7, ADR-0018 §10) on a fake clock: a window of 2 s quiet or 30 s at most, one
// refresh a minute while a burst lasts, nothing while a game runs, in the editor or during a ride (the window opening
// again at the return), every system after an overflow, a root lost and back; and LibraryWatcher over a fake watcher:
// the changes mapped (an emulator's save beside a ROM ignored), the roots armed once, the request it makes (the trigger
// watcher, the systems, never removals), the run report's record.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/library_watch.hpp>
#include <rv/app/library_watcher.hpp>
#include <rv/app/run_report.hpp>
#include <rv/esde/change_scope.hpp>

#include "support/fake_change_watcher.hpp"

namespace {

using namespace std::chrono_literals;
using rv::app::LibraryWatch;
using rv::app::WatchRefresh;
using Clock = std::chrono::steady_clock;

const Clock::time_point t0 = Clock::time_point{} + std::chrono::hours(1);

Clock::time_point at(std::chrono::milliseconds offset) {
    return t0 + offset;
}

} // namespace

TEST_CASE("app/library_watch: a change waits 2 s of quiet, then one refresh of its system", "[app][library_watch]") {
    LibraryWatch watch;
    CHECK_FALSE(watch.due(at(0ms), false).has_value());
    CHECK_FALSE(watch.nextDue().has_value());
    watch.change(at(0ms), std::string("snes"));
    CHECK(watch.pending());
    CHECK(watch.nextDue() == at(2000ms));
    CHECK_FALSE(watch.due(at(1999ms), false).has_value());
    // Another change pushes the end of the window.
    watch.change(at(1500ms), std::string("gb"));
    CHECK_FALSE(watch.due(at(3000ms), false).has_value());
    CHECK(watch.nextDue() == at(3500ms));
    const std::optional<WatchRefresh> due = watch.due(at(3500ms), false);
    REQUIRE(due.has_value());
    CHECK(due->systems == std::vector<std::string>{"gb", "snes"});
    CHECK_FALSE(due->everything);
    CHECK(due->changes == 2);
    CHECK(due->firstChange == at(0ms));
    CHECK_FALSE(due->held);
    CHECK_FALSE(due->burst);
    // Due until started: a refresh the catalog refused is asked again.
    CHECK(watch.due(at(3600ms), false).has_value());
    watch.started(at(3600ms));
    CHECK_FALSE(watch.pending());
    CHECK_FALSE(watch.due(at(10000ms), false).has_value());
}

TEST_CASE("app/library_watch: a burst: 30 s at most, then one refresh a minute until it ends in quiet",
          "[app][library_watch]") {
    LibraryWatch watch;
    // ES-DE scraping: a file every 500 ms for three minutes.
    std::vector<Clock::time_point> refreshes;
    std::vector<bool> bursts;
    for (std::chrono::milliseconds now = 0ms; now <= 240000ms; now += 100ms) {
        if (now <= 180000ms && now.count() % 500 == 0) {
            watch.change(at(now), std::string("snes"));
        }
        if (const std::optional<WatchRefresh> due = watch.due(at(now), false)) {
            refreshes.push_back(at(now));
            bursts.push_back(due->burst);
            watch.started(at(now));
        }
    }
    // At 30 s (the window's longest), then every 60 s while it goes on (90, 150 s), then 2 s after the last change.
    REQUIRE(refreshes.size() == 4);
    CHECK(refreshes[0] == at(30000ms));
    CHECK(refreshes[1] == at(90000ms));
    CHECK(refreshes[2] == at(150000ms));
    CHECK(refreshes[3] == at(182000ms));
    CHECK(bursts == std::vector<bool>{true, true, true, false});
}

TEST_CASE("app/library_watch: two changes 20 s apart are two refreshes, each 2 s after its change",
          "[app][library_watch]") {
    LibraryWatch watch;
    watch.change(at(0ms), std::string("snes"));
    REQUIRE(watch.due(at(2000ms), false).has_value());
    watch.started(at(2000ms));
    watch.change(at(20000ms), std::string("snes"));
    CHECK_FALSE(watch.due(at(21000ms), false).has_value());
    CHECK(watch.due(at(22000ms), false).has_value()); // no minute's wait: no burst
}

TEST_CASE("app/library_watch: nothing starts while held; the window opens again at the return",
          "[app][library_watch]") {
    LibraryWatch watch;
    // A game runs (or the editor is open, or a ride): changes come and wait.
    CHECK_FALSE(watch.due(at(0ms), true).has_value());
    watch.change(at(1000ms), std::string("snes"));
    watch.change(at(2000ms), std::nullopt); // a gamelist's overflow: every system
    for (std::chrono::milliseconds now = 2000ms; now <= 120000ms; now += 1000ms) {
        CHECK_FALSE(watch.due(at(now), true).has_value());
    }
    CHECK_FALSE(watch.nextDue().has_value());
    CHECK(watch.heldChanges() == 2);
    // The game returned at 120 s: the window opens then, 2 s of quiet from the return.
    CHECK_FALSE(watch.due(at(120000ms), false).has_value());
    CHECK_FALSE(watch.due(at(121999ms), false).has_value());
    const std::optional<WatchRefresh> due = watch.due(at(122000ms), false);
    REQUIRE(due.has_value());
    CHECK(due->everything);
    CHECK(due->systems.empty());
    CHECK(due->held);
    CHECK(due->firstChange == at(1000ms));
    watch.started(at(122000ms));
    // A change that comes before the hold and waits through it counts as held too.
    watch.change(at(130000ms), std::string("gb"));
    CHECK_FALSE(watch.due(at(130500ms), true).has_value());
    CHECK(watch.heldChanges() == 3);
    const std::optional<WatchRefresh> after = watch.due(at(140000ms), false);
    CHECK_FALSE(after.has_value()); // the window opened at 140 s
    REQUIRE(watch.due(at(142000ms), false).has_value());
    CHECK(watch.due(at(142000ms), false)->held);
}

TEST_CASE("app/library_watch: a root lost and back, an overflow: every system", "[app][library_watch]") {
    LibraryWatch watch;
    watch.change(at(0ms), std::string("snes"));
    watch.change(at(100ms), std::nullopt); // the ROM root lost
    const std::optional<WatchRefresh> lost = watch.due(at(2100ms), false);
    REQUIRE(lost.has_value());
    CHECK(lost->everything);
    CHECK(lost->systems.empty());
    watch.started(at(2100ms));
    watch.change(at(30000ms), std::nullopt); // back
    const std::optional<WatchRefresh> back = watch.due(at(32000ms), false);
    REQUIRE(back.has_value());
    CHECK(back->everything);
}

namespace {

/// A library's map as the worker makes it, with invented folders.
rv::esde::ChangeScopeMap invented() {
    rv::esde::ChangeScopeMap map;
    map.esdeHome = "X:\\Invented\\ES-DE";
    map.romRoot = std::filesystem::path("Y:\\Roms");
    map.mediaRoot = std::filesystem::path("X:\\Invented\\Media");
    rv::esde::ScopeSystem snes;
    snes.name = rv::catalog::SystemName::parse("snes").value();
    snes.romFolder = std::filesystem::path("Y:\\Roms\\snes");
    snes.extensions = {".sfc", ".SFC", ".zip"};
    rv::esde::ScopeSystem gb;
    gb.name = rv::catalog::SystemName::parse("gb").value();
    gb.romFolder = std::filesystem::path("Y:\\Roms\\gb");
    gb.extensions = {".gb", ".zip"};
    map.systems = {gb, snes};
    return map;
}

} // namespace

TEST_CASE("app/library_watcher: changes mapped through the scope, the request partial and never accepting removals",
          "[app][library_watch]") {
    rv::test::FakeChangeWatcher fake;
    rv::app::LibraryWatcher watcher(fake);
    // Changes before the map are no one's.
    fake.push("rom", rv::core::ChangeKind::Added, "Y:\\Roms\\snes\\Early (USA).sfc");
    CHECK_FALSE(watcher.poll(at(0ms), false).has_value());

    watcher.arm(invented());
    REQUIRE(fake.watched() == 1);
    const std::vector<rv::core::WatchRoot> roots = fake.roots();
    std::vector<std::string> names;
    for (const rv::core::WatchRoot& root : roots) {
        names.push_back(root.name);
    }
    CHECK(names == std::vector<std::string>{"settings", "custom-systems", "gamelists", "rom", "media"});
    // The same map again: nothing asked again.
    watcher.arm(invented());
    CHECK(fake.watched() == 1);

    fake.push("rom", rv::core::ChangeKind::Added, "Y:\\Roms\\snes\\New Find (USA).sfc");
    fake.push("rom", rv::core::ChangeKind::Added, "Y:\\Roms\\snes\\New Find (USA).srm");  // a save: nothing
    fake.push("rom", rv::core::ChangeKind::Modified, "Y:\\Roms\\gb\\saves\\Tiny.state1"); // nothing
    fake.push("media", rv::core::ChangeKind::Added, "X:\\Invented\\Media\\gb\\covers\\Tiny (World).png");
    fake.push("media", rv::core::ChangeKind::Added, "X:\\Invented\\Media\\gb\\covers\\Thumbs.db"); // nothing
    CHECK_FALSE(watcher.poll(at(100ms), false).has_value());
    const std::optional<WatchRefresh> due = watcher.poll(at(2100ms), false);
    REQUIRE(due.has_value());
    CHECK(due->systems == std::vector<std::string>{"gb", "snes"});
    const rv::app::RefreshRequest request = rv::app::LibraryWatcher::requestOf(*due);
    CHECK(request.trigger == rv::app::RefreshTrigger::Watcher);
    REQUIRE(request.systems.size() == 2);
    CHECK(request.systems[0].str() == "gb");
    CHECK(request.systems[1].str() == "snes");
    CHECK_FALSE(request.acceptRemovals);
    // Refused while a refresh runs: asked again, counted once.
    watcher.refused();
    watcher.refused();
    REQUIRE(watcher.poll(at(2200ms), false).has_value());
    watcher.started(at(2200ms));
    CHECK_FALSE(watcher.poll(at(9000ms), false).has_value());

    // An overflow, then a root lost: every system.
    fake.push("rom", rv::core::ChangeKind::Overflow, "Y:\\Roms");
    fake.push("media", rv::core::ChangeKind::RootLost, "X:\\Invented\\Media");
    CHECK_FALSE(watcher.poll(at(12000ms), false).has_value());
    const std::optional<WatchRefresh> wide = watcher.poll(at(14000ms), false);
    REQUIRE(wide.has_value());
    CHECK(wide->everything);
    CHECK(rv::app::LibraryWatcher::requestOf(*wide).systems.empty());
    watcher.started(at(14000ms));

    fake.setMode("media", rv::core::WatchMode::Lost);
    rv::app::WatcherRecord record;
    watcher.record(record);
    CHECK(record.enabled);
    REQUIRE(record.roots.size() == 5);
    CHECK(record.roots[3].name == "rom");
    CHECK(record.roots[3].mode == "notify");
    CHECK(record.roots[4].mode == "lost");
    CHECK(record.changes == 8);
    CHECK(record.mapped == 4);
    CHECK(record.ignored == 4);
    CHECK(record.overflows == 1);
    CHECK(record.rootsLost == 1);
    CHECK(record.refreshes == 2);
    CHECK(record.refused == 1);
    std::vector<std::string> reasons;
    for (const rv::app::WatchReasonCount& reason : record.ignoredBy) {
        reasons.push_back(reason.reason + "=" + std::to_string(reason.count));
    }
    CHECK(reasons == std::vector<std::string>{"extension=2", "no-map=1", "not-media=1"});
}
