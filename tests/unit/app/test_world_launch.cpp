// Launching from the world and "Mostra nella stanza" (I3 step 15a; ADR-0012 §6, §8; rv/app/world_launch_ui.hpp):
// where a game stands in the room as the quick catalog's card words it (the plan's where-is index: placed, each
// unplaced reason, not in the room), the words of the quick catalog's document for each key and its button, the busy
// refusal, and the run report's world.launch record kept as the app's loop drives the interaction through a launch, a
// refusal and "Mostra nella stanza" (rv_app's WorldLaunch does the same around ProductUi).

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <rv/app/launch_outcome.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_launch_ui.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action.hpp>
#include <rv/ui/quick_catalog_room.hpp>
#include <rv/ui/ui_assets.hpp>
#include <rv/world/where_is.hpp>

#include "../world/world_test_plans.hpp"
#include "world_interaction_harness.hpp"

namespace qc = rv::ui::quick_catalog;
using rv::app::RoomLaunchLog;
using rv::app::WorldState;
using rv::input::Action;
using rv::test::InteractionHarness;
using rv::test::pressing;

namespace {

std::string readAsset(std::string_view relative) {
    const std::filesystem::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto text = rv::core::readTextFile(assets / rv::core::pathFromUtf8(relative));
    REQUIRE(text.has_value());
    return *text;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

/// Inspect of module 0's level 4, slot 16 (as test_world_interaction.cpp's inspecting()), the item in the hand.
InteractionHarness inspecting() {
    rv::test::ShelfRoom probe(rv::test::harnessShelves());
    InteractionHarness harness(rv::test::facing(probe.shelves().item(probe.at(0, 4, 16)), 1.2f));
    harness.idle(3);
    REQUIRE(harness.interaction.hovered() == harness.room.at(0, 4, 16));
    harness.frame(pressing({Action::Interact}));
    harness.idle(30);
    harness.frame(pressing({Action::UiActivate}));
    REQUIRE(harness.interaction.state() == WorldState::Inspect);
    harness.idle(30);
    REQUIRE(harness.interaction.inspectView().lift == 1.0f);
    return harness;
}

std::vector<std::string> eventsOf(const rv::app::RoomLaunchRecord& record) {
    std::vector<std::string> events;
    for (const rv::app::RoomLaunchEventRecord& event : record.events) {
        events.push_back(event.event);
    }
    return events;
}

} // namespace

TEST_CASE("app/world_launch: where a game stands in the room, as the quick catalog's card words it",
          "[app][world_launch]") {
    rv::world::WorldPlan plan = rv::test::validRoomPlan();
    REQUIRE_FALSE(plan.placements.empty());
    // The plan's own unplaced reasons, and the two it has no game for, added by hand (the index takes any plan).
    const rv::world::RoomId roomId = plan.rooms.front().id;
    plan.unplaced.push_back({rv::test::testGameId(900001), roomId, rv::world::UnplacedReason::Unavailable, {}});
    plan.unplaced.push_back({rv::test::testGameId(900002), roomId, rv::world::UnplacedReason::Capacity, {}});
    const rv::world::PlanIndex index(plan);

    const rv::catalog::GameId placed = plan.placements.front().game;
    CHECK(rv::app::roomPlacementOf(index, placed, true) == qc::kRoomPlaced);
    // Placed by the plan, but the room shown has no item of it: not offered.
    CHECK(rv::app::roomPlacementOf(index, placed, false) == qc::kRoomNotInRoom);
    std::vector<std::string_view> reasons;
    for (const rv::world::UnplacedGame& unplaced : plan.unplaced) {
        const std::string_view key = rv::app::roomPlacementOf(index, unplaced.game, false);
        CHECK(key == rv::world::toString(unplaced.reason));
        CHECK(std::find(qc::kRoomUnplacedKeys.begin(), qc::kRoomUnplacedKeys.end(), key) !=
              qc::kRoomUnplacedKeys.end());
        reasons.push_back(key);
    }
    for (const std::string_view reason : {"duplicate-title", "no-cover", "unavailable", "capacity"}) {
        CHECK(std::find(reasons.begin(), reasons.end(), reason) != reasons.end());
    }
    // A game the plan does not hold (another system's).
    CHECK(rv::app::roomPlacementOf(index, rv::test::testGameId(900003), false) == qc::kRoomNotInRoom);

    // The world's reasons are the document's keys, in order, then "not in the room".
    using rv::world::UnplacedReason;
    const UnplacedReason all[] = {UnplacedReason::DuplicateTitle, UnplacedReason::NoCover, UnplacedReason::Unavailable,
                                  UnplacedReason::Capacity};
    for (std::size_t i = 0; i < std::size(all); ++i) {
        CHECK(rv::world::toString(all[i]) == qc::kRoomUnplacedKeys[i]);
    }
    CHECK(qc::kRoomUnplacedKeys.back() == qc::kRoomNotInRoom);
}

TEST_CASE("app/world_launch: the quick catalog's card offers Mostra nella stanza and says why a game is not there",
          "[app][world_launch][ui]") {
    const std::string rml = readAsset(qc::kDocumentPath);
    const std::string rcss = readAsset(qc::kStyleSheetPath);
    // The button, in the card's actions region, only for a placed game, pressing the screen's data event.
    const std::size_t actions = rml.find("nav-region=\"actions\"");
    const std::size_t button = rml.find("id=\"" + std::string(qc::kShowInRoomId) + "\"");
    REQUIRE(actions != std::string::npos);
    REQUIRE(button != std::string::npos);
    CHECK(button > actions);
    CHECK(rml.find("</div>", actions) > rml.find("act-info", actions)); // after Informazioni
    CHECK(contains(rml, "data-if=\"card.room == '" + std::string(qc::kRoomPlaced) + "'\""));
    CHECK(
        contains(rml, "data-event-click=\"" + std::string(qc::kShowInRoomEvent) + "()\">Mostra nella stanza</button>"));
    // A sentence for each reason, in Italian, starting the same way.
    for (const std::string_view key : qc::kRoomUnplacedKeys) {
        const std::string condition = "data-if=\"card.room == '" + std::string(key) + "'\">Non esposto: ";
        INFO(key);
        CHECK(contains(rml, condition));
    }
    CHECK(contains(rml, "Non esposto: un'altra versione è sullo scaffale"));
    CHECK(contains(rml, "Non esposto: senza copertina"));
    CHECK(contains(rml, "Non esposto: la stanza è piena"));
    CHECK(contains(rcss, ".room-reason"));
    CHECK_FALSE(contains(rcss, "transition:"));
}

TEST_CASE("app/world_launch: an Avvia the product UI cannot take ends at once, not ready and busy",
          "[app][world_launch]") {
    const rv::app::LaunchMessage busy = rv::app::busyLaunchRefusal();
    CHECK(busy.code == rv::app::launch_outcome::kNotReady);
    CHECK(busy.reason == rv::app::kReasonLaunchBusy);
    CHECK(rv::app::showsMessage(busy));
}

TEST_CASE("app/world_launch: the record follows a launch to its return, the walker restored, and a refusal",
          "[app][world_launch]") {
    InteractionHarness harness = inspecting();
    const rv::world::PickId item = harness.room.at(0, 4, 16);
    RoomLaunchLog log;
    log.bind();
    const rv::scene::WalkerPose atLaunch = harness.pose();

    // Avvia: the app asks the product UI, the interaction waits.
    const rv::app::InteractionResult launch = harness.press(rv::app::CardAction::Launch);
    REQUIRE(launch.launch.has_value());
    log.launchAsked(*launch.launch, harness.interaction.launchRecord()->walker, 100, true);
    log.observe(harness.interaction.state(), harness.interaction.focused(), harness.pose(), 100);
    // The game runs: frames with the product UI's input, the walker moved meanwhile (a window handed over and back).
    harness.frame(nullptr);
    log.observe(harness.interaction.state(), harness.interaction.focused(), harness.pose(), 101);
    harness.place(rv::test::facing(harness.room.shelves().item(harness.room.at(1, 2, 3)), 2.0f));
    // The return: the end given to the interaction before the frame's update, which restores pose and Inspect.
    const rv::app::LaunchMessage returned{std::string(rv::app::launch_outcome::kReturned), "", ""};
    log.launchEnded(returned, 102);
    harness.interaction.launchEnded(returned);
    const rv::app::InteractionResult back = harness.idle(1);
    REQUIRE(back.walker.has_value());
    log.observe(harness.interaction.state(), harness.interaction.focused(), *back.walker, 102);

    const rv::app::RoomLaunchRecord& record = log.record();
    CHECK(record.bound);
    CHECK(record.launches == 1);
    CHECK(record.returned == 1);
    CHECK(record.notReturned == 0);
    CHECK(eventsOf(record) == std::vector<std::string>{"launch", "ended", "restored"});
    CHECK(record.events[0].game == rv::test::ShelfRoom::gameOf(item).toString());
    CHECK(record.events[0].pickId == fmt::format("{:08X}", item));
    CHECK(record.events[1].outcome == "returned");
    CHECK(record.events[2].state == "inspect");
    CHECK(record.events[2].pickId == fmt::format("{:08X}", item));
    CHECK(record.poseRestored);
    REQUIRE(record.walkerBefore.has_value());
    REQUIRE(record.walkerAfter.has_value());
    CHECK(*record.walkerBefore == rv::app::spawnOf(atLaunch));
    CHECK(*record.walkerAfter == *record.walkerBefore);
    CHECK(harness.interaction.state() == WorldState::Inspect);
    CHECK(harness.interaction.focused() == item);

    // A refusal (the product UI busy): ended at once, back to Inspect on the next frame with the card's notice.
    const rv::app::InteractionResult again = harness.press(rv::app::CardAction::Launch);
    REQUIRE(again.launch.has_value());
    log.launchAsked(*again.launch, harness.interaction.launchRecord()->walker, 110, false);
    log.launchEnded(rv::app::busyLaunchRefusal(), 110);
    harness.interaction.launchEnded(rv::app::busyLaunchRefusal());
    log.observe(harness.interaction.state(), harness.interaction.focused(), harness.pose(), 110);
    const rv::app::InteractionResult refused = harness.idle(1);
    log.observe(harness.interaction.state(), harness.interaction.focused(), refused.walker.value_or(harness.pose()),
                111);
    CHECK(harness.interaction.state() == WorldState::Inspect);
    CHECK(harness.interaction.notice() == rv::app::InteractionNotice::LaunchOutcome);
    CHECK(harness.interaction.noticeMessage().reason == rv::app::kReasonLaunchBusy);
    CHECK(record.launches == 2);
    CHECK(record.notReturned == 1);
    CHECK(eventsOf(record) ==
          std::vector<std::string>{"launch", "ended", "restored", "launch", "refused", "ended", "restored"});
    CHECK(record.events.back().frame == 111);
    CHECK(record.poseRestored);
}

TEST_CASE("app/world_launch: Mostra nella stanza eases the walker to the item's Focus pose, recorded",
          "[app][world_launch]") {
    rv::test::ShelfRoom probe(rv::test::harnessShelves());
    // From the far end of the room, looking elsewhere.
    InteractionHarness harness(rv::test::facing(probe.shelves().item(probe.at(1, 0, 0)), 2.4f));
    const rv::world::PickId target = harness.room.at(0, 2, 7);
    RoomLaunchLog log;
    log.bind();
    REQUIRE(harness.interaction.showInRoom(target, harness.pose()));
    log.showInRoom(rv::test::ShelfRoom::gameOf(target), target, 5, {});
    CHECK(harness.interaction.state() == WorldState::Focus);
    CHECK(harness.interaction.focused() == target);
    // 600 ms at 60 fps, then the walker stands at the item's Focus pose, the item out.
    harness.idle(40);
    CHECK_FALSE(harness.interaction.walkerEasing());
    CHECK(rv::scene::samePose(harness.pose(), harness.room.focusPose(target)));
    CHECK(harness.interaction.pullOf(target) == 1.0f);
    // An item the room no longer holds: refused, said why.
    harness.room.remove(harness.room.at(0, 2, 8));
    CHECK_FALSE(harness.interaction.showInRoom(harness.room.at(0, 2, 8), harness.pose()));
    CHECK(harness.interaction.notice() == rv::app::InteractionNotice::ItemUnavailable);
    log.showInRoom(rv::test::ShelfRoom::gameOf(harness.room.at(0, 2, 8)), rv::world::kNoPick, 50, "item-unavailable");
    CHECK(log.record().showInRoom == 1);
    CHECK(log.record().showInRoomFailed == 1);
    CHECK(eventsOf(log.record()) == std::vector<std::string>{"show-in-room", "show-in-room-failed"});
    CHECK(log.record().events.back().reason == "item-unavailable");
}

TEST_CASE("app/world_launch: the record reads back from the run report", "[app][world_launch][run_report]") {
    RoomLaunchLog log;
    log.bind();
    rv::app::WorldLaunchRequest request{0x01000203u, rv::test::testGameId(4242)};
    const rv::scene::WalkerPose walker{glm::vec3(1.25f, 0.0f, -2.5f), 0.5f, -0.25f};
    log.launchAsked(request, walker, 7, true);
    log.launchEnded(rv::app::LaunchMessage{"returned", "", ""}, 9);
    log.observe(WorldState::Inspect, request.item, walker, 9);
    log.showInRoom(request.game, request.item, 12, {});

    rv::app::RunReport report;
    report.world.bound = true;
    report.world.state = "room";
    report.world.launch = log.record();
    auto read = rv::app::parseRunReport(rv::app::formatRunReport(report), "test");
    REQUIRE(read.has_value());
    CHECK(read->world.launch == report.world.launch);
}
