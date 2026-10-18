// I3 step 7: app::WorldInteraction driven by input scripts (rv::input::InputScript, the format of --input-script) on
// hand-built shelves, frame by frame at a fixed frame time, as a scripted run of the app will drive it: the states and
// items each step reaches, and two runs identical to the bit.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/app/world_interaction.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>

#include "world_interaction_harness.hpp"

using rv::app::WorldState;
using rv::input::InputScript;
using rv::input::InputScriptPlayer;
using rv::test::FrameRecord;
using rv::test::InteractionHarness;

namespace {

/// From FreeRoam in front of module 0's level 4, slot 16: Focus, two steps right, one up, Inspect, a turn and a zoom,
/// the card's Indietro, Back, then a walk.
constexpr std::string_view kTour = R"([
  {"fromFrame": 20, "toFrame": 20, "actions": {"interact": 1}},
  {"fromFrame": 50, "toFrame": 50, "actions": {"ui_right": 1}},
  {"fromFrame": 60, "toFrame": 60, "actions": {"ui_right": 1}},
  {"fromFrame": 70, "toFrame": 70, "actions": {"ui_up": 1}},
  {"fromFrame": 100, "toFrame": 100, "actions": {"ui_activate": 1}},
  {"fromFrame": 130, "toFrame": 159, "actions": {"look_x": 1.0}},
  {"fromFrame": 160, "toFrame": 171, "actions": {"ui_page_down": 1.0}},
  {"fromFrame": 180, "toFrame": 180, "actions": {"ui_back": 1}},
  {"fromFrame": 200, "toFrame": 200, "actions": {"back": 1}},
  {"fromFrame": 230, "toFrame": 259, "actions": {"move_forward": -1.0}}
])";

/// Runs `script` for `frames` frames; one record per frame.
std::vector<FrameRecord> play(std::string_view script, int frames) {
    const auto parsed = InputScript::fromJson(nlohmann::json::parse(script), "tour");
    REQUIRE(parsed.has_value());
    InputScriptPlayer player(*parsed);
    rv::test::ShelfRoom probe(rv::test::harnessShelves());
    InteractionHarness harness(rv::test::facing(probe.shelves().item(probe.at(0, 4, 16)), 1.2f));
    std::vector<FrameRecord> records;
    for (int frame = 0; frame < frames; ++frame) {
        rv::input::ActionState actions;
        player.apply(static_cast<std::uint64_t>(frame), actions, rv::input::InputSettings{}.pressThreshold);
        harness.frame(actions);
        records.push_back(harness.record());
    }
    return records;
}

} // namespace

TEST_CASE("app/world_interaction: a scripted tour reaches each state on its frame", "[app][world_interaction][script]") {
    const std::vector<FrameRecord> records = play(kTour, 270);
    rv::test::ShelfRoom room(rv::test::harnessShelves());
    const auto id = [&](int module, int level, int slot) { return room.at(module, level, slot); };

    // Hovered on the third frame, pulled out from the dwell's end (frame 10) for 15 frames.
    CHECK(records[1].hovered == rv::world::kNoPick);
    CHECK(records[2].hovered == id(0, 4, 16));
    CHECK(records[9].pulled.empty());
    REQUIRE(records[10].pulled.size() == 1);
    CHECK(records[10].pulled[0].id == id(0, 4, 16));
    CHECK(records[19].pulled[0].progress < 1.0f);
    CHECK(records[19].state == WorldState::FreeRoam);
    CHECK(records[24].pulled == std::vector<rv::app::PulledItem>{{id(0, 4, 16), 1.0f}});
    // Focus at frame 20, the walker eased for 21 frames.
    CHECK(records[20].state == WorldState::Focus);
    CHECK(records[20].walkerDriven);
    CHECK(records[40].walkerDriven);
    CHECK_FALSE(records[41].walkerDriven);
    CHECK(rv::scene::samePose(records[41].walker, room.focusPose(id(0, 4, 16))));
    // Two steps right and one up.
    CHECK(records[50].focused == id(0, 4, 17));
    CHECK(records[60].focused == id(0, 4, 18));
    CHECK(records[70].focused == id(0, 5, 18));
    CHECK(records[99].focused == id(0, 5, 18));
    CHECK(rv::scene::samePose(records[99].walker, room.focusPose(id(0, 5, 18))));
    // Inspect at frame 100, lifted in 18 frames; turned for 30 frames, zoomed for 12, the walker still.
    CHECK(records[100].state == WorldState::Inspect);
    CHECK(records[116].inspect.lift < 1.0f);
    CHECK(records[117].inspect.lift == 1.0f);
    CHECK(records[159].inspect.yaw > 1.5f);
    CHECK(records[171].inspect.distance < 0.51f);
    CHECK(rv::scene::samePose(records[179].walker, records[99].walker));
    // Indietro: Focus, lowered; Back: FreeRoam, the item put back.
    CHECK(records[180].state == WorldState::Focus);
    CHECK(records[192].inspect.lift == 0.0f);
    CHECK(records[200].state == WorldState::FreeRoam);
    CHECK(records[200].focused == rv::world::kNoPick);
    CHECK(records[229].pulled.empty());
    // Walking again (backwards, away from the shelf).
    CHECK(records[259].walker.feet.z > records[229].walker.feet.z + 0.5f);
    CHECK(records[259].state == WorldState::FreeRoam);
}

TEST_CASE("app/world_interaction: two scripted runs are identical", "[app][world_interaction][script]") {
    const std::vector<FrameRecord> first = play(kTour, 270);
    const std::vector<FrameRecord> second = play(kTour, 270);
    REQUIRE(first.size() == second.size());
    for (std::size_t frame = 0; frame < first.size(); ++frame) {
        CAPTURE(frame);
        CHECK(first[frame] == second[frame]);
    }
}

TEST_CASE("app/world_interaction: a still stick held for 60 frames never changes the hover or the focus",
          "[app][world_interaction][script]") {
    // The right stick resting inside its dead zone reads 0; a held mouse moves nothing.
    constexpr std::string_view kStill = R"([
      {"fromFrame": 0, "toFrame": 9, "actions": {"look_x": 0.0, "look_y": 0.0}},
      {"fromFrame": 10, "toFrame": 10, "actions": {"interact": 1}},
      {"fromFrame": 40, "toFrame": 99, "actions": {"look_x": 0.0, "look_delta_x": 0.0}}
    ])";
    const std::vector<FrameRecord> records = play(kStill, 100);
    for (int frame = 2; frame < 10; ++frame) {
        CHECK(records[static_cast<std::size_t>(frame)].hovered == records[2].hovered);
    }
    for (int frame = 40; frame < 100; ++frame) {
        CAPTURE(frame);
        CHECK(records[static_cast<std::size_t>(frame)].state == WorldState::Focus);
        CHECK(records[static_cast<std::size_t>(frame)].focused == records[40].focused);
        CHECK(records[static_cast<std::size_t>(frame)].hovered == records[40].hovered);
    }
}
