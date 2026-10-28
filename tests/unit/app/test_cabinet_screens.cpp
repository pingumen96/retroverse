// Which arcade cabinets play their clips (I5 step 4, ADR-0016 §6): attract on the nearest cabinet in view within 3 m
// whose screen faces the eye, after a 1 s dwell; the focused (focus) or inspected (direct) one; none in the editor.

#include <rv/app/cabinet_screens.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

#include <glm/vec3.hpp>

using rv::app::CabinetAttract;
using rv::app::CabinetCandidate;
using rv::app::CabinetChoice;
using rv::app::CabinetPlay;
using rv::app::chooseCabinets;
using rv::app::nearestAttract;
using rv::app::ScreenClass;
using rv::app::WorldState;
using rv::world::PickId;

namespace {

/// The run's step under --fixed-dt 0.016667.
constexpr double kFrame = 0.016667;

/// A row of cabinets along the west wall (x = -4), their screens facing east (+X), 0.46 m apart from z = 0 (picks 1,
/// 2, 3, ...), at a screen's height.
std::vector<CabinetCandidate> westRow(int count) {
    std::vector<CabinetCandidate> row;
    for (int index = 0; index < count; ++index) {
        row.push_back(CabinetCandidate{static_cast<PickId>(index + 1),
                                       glm::vec3(-4.0f, 1.3f, -0.46f * static_cast<float>(index)),
                                       glm::vec3(1.0f, 0.0f, 0.0f), true});
    }
    return row;
}

/// `frames` frames of `attract` with the eye at `eye`; what played on the last, the time after.
std::optional<PickId> walk(CabinetAttract& attract, const std::vector<CabinetCandidate>& candidates,
                           const glm::vec3& eye, int frames, double& now) {
    std::optional<PickId> playing;
    for (int frame = 0; frame < frames; ++frame) {
        playing = attract.update(candidates, eye, now);
        now += kFrame;
    }
    return playing;
}

} // namespace

TEST_CASE("app/cabinets: attract's choice: the nearest cabinet in view within 3 m whose screen faces the eye",
          "[app][screens][cabinets]") {
    const std::vector<CabinetCandidate> row = westRow(4);
    // 1.5 m in front of cabinet 2 (z = -0.46): it is the nearest, facing the eye.
    CHECK(nearestAttract(row, {-2.5f, 1.6f, -0.46f}) == std::optional<PickId>(2));
    // In front of the gap between 1 and 2, the same distance to both: the smaller pick id.
    CHECK(nearestAttract(row, {-2.5f, 1.6f, -0.23f}) == std::optional<PickId>(1));
    // 3.2 m away: beyond 3 m, none; 2.9 m: the nearest.
    CHECK_FALSE(nearestAttract(row, {-0.8f, 1.3f, 0.0f}).has_value());
    CHECK(nearestAttract(row, {-1.1f, 1.3f, 0.0f}) == std::optional<PickId>(1));
    // Behind the screens (the wall's other side): none.
    CHECK_FALSE(nearestAttract(row, {-5.0f, 1.3f, 0.0f}).has_value());
    // Out of view: never, however near.
    std::vector<CabinetCandidate> hidden = row;
    for (CabinetCandidate& candidate : hidden) {
        candidate.visible = false;
    }
    CHECK_FALSE(nearestAttract(hidden, {-2.5f, 1.6f, -0.46f}).has_value());
    // The one playing keeps it to 3.5 m and 70 degrees off its normal; another needs 3 m and 60.
    CHECK_FALSE(nearestAttract(row, {-0.75f, 1.3f, 0.0f}).has_value());
    CHECK(nearestAttract(row, {-0.75f, 1.3f, 0.0f}, 1) == std::optional<PickId>(1));
    const glm::vec3 oblique{-4.0f + 1.5f * 0.5f, 1.3f, -1.5f * 0.866f}; // 60 degrees off cabinet 1's normal, to its north
    const glm::vec3 steeper{-4.0f + 1.5f * 0.4f, 1.3f, -1.5f * 0.917f}; // 66 degrees
    std::vector<CabinetCandidate> one{row.front()};
    CHECK(nearestAttract(one, oblique) == std::optional<PickId>(1));
    CHECK_FALSE(nearestAttract(one, steeper).has_value());
    CHECK(nearestAttract(one, steeper, 1) == std::optional<PickId>(1));
}

TEST_CASE("app/cabinets: attract plays after its 1 s dwell and follows the eye along a row", "[app][screens][cabinets]") {
    const std::vector<CabinetCandidate> row = westRow(4);
    CabinetAttract attract;
    double now = 0.0;
    // In front of cabinet 1: nothing for 59 frames (0.98 s), cabinet 1 at the 61st (1.0 s after the first).
    CHECK_FALSE(walk(attract, row, {-2.5f, 1.6f, 0.0f}, 60, now).has_value());
    CHECK(attract.candidate() == std::optional<PickId>(1));
    CHECK(walk(attract, row, {-2.5f, 1.6f, 0.0f}, 1, now) == std::optional<PickId>(1));
    // A step to cabinet 3: cabinet 1 plays on (it still qualifies) while 3's dwell runs, then 3 takes over.
    CHECK(walk(attract, row, {-2.5f, 1.6f, -0.92f}, 30, now) == std::optional<PickId>(1));
    CHECK(attract.candidate() == std::optional<PickId>(3));
    CHECK(walk(attract, row, {-2.5f, 1.6f, -0.92f}, 31, now) == std::optional<PickId>(3));
    // Passing by quicker than the dwell: 3 plays on through the half-second in front of 4.
    CHECK(walk(attract, row, {-2.5f, 1.6f, -1.38f}, 30, now) == std::optional<PickId>(3));
    CHECK(walk(attract, row, {-2.5f, 1.6f, -0.92f}, 30, now) == std::optional<PickId>(3));
    CHECK_FALSE(attract.candidate().has_value());
    // Walking away past 3.5 m: it stops at once, and nothing plays.
    CHECK_FALSE(walk(attract, row, {0.0f, 1.6f, -0.92f}, 1, now).has_value());
    // Back within 3 m: the dwell again from its start.
    CHECK_FALSE(walk(attract, row, {-2.5f, 1.6f, -0.92f}, 60, now).has_value());
    CHECK(walk(attract, row, {-2.5f, 1.6f, -0.92f}, 1, now) == std::optional<PickId>(3));
    // Turned away (its box out of view): it stops at once; reset() forgets the dwell too.
    std::vector<CabinetCandidate> hidden = row;
    for (CabinetCandidate& candidate : hidden) {
        candidate.visible = false;
    }
    CHECK_FALSE(walk(attract, hidden, {-2.5f, 1.6f, -0.92f}, 1, now).has_value());
    CHECK(walk(attract, row, {-2.5f, 1.6f, 0.0f}, 30, now) == std::nullopt);
    attract.reset();
    CHECK_FALSE(attract.candidate().has_value());
    CHECK_FALSE(walk(attract, row, {-2.5f, 1.6f, 0.0f}, 60, now).has_value());
    CHECK(walk(attract, row, {-2.5f, 1.6f, 0.0f}, 1, now) == std::optional<PickId>(1));
}

TEST_CASE("app/cabinets: the focused cabinet plays in focus, the inspected one direct, attract only in free roam",
          "[app][screens][cabinets]") {
    CabinetChoice choice;
    choice.attractPick = 7;
    // Free roam: the attract cabinet, ambient.
    CHECK(chooseCabinets(choice) == std::vector<CabinetPlay>{{7, ScreenClass::Ambient}});
    // Focus on cabinet 3: it alone, focus; Inspect and a launch's states: direct (its clip's position kept for the
    // return).
    choice.focused = 3;
    choice.state = WorldState::Focus;
    CHECK(chooseCabinets(choice) == std::vector<CabinetPlay>{{3, ScreenClass::Focus}});
    for (const WorldState state : {WorldState::Inspect, WorldState::Launching, WorldState::Returning}) {
        choice.state = state;
        CHECK(chooseCabinets(choice) == std::vector<CabinetPlay>{{3, ScreenClass::Direct}});
    }
    // video.json's cabinet showOnFocus off: the focused cabinet shows its cover, and no attract out of free roam.
    choice.showOnFocus = false;
    CHECK(chooseCabinets(choice).empty());
    choice.state = WorldState::FreeRoam;
    choice.focused = rv::world::kNoPick;
    CHECK(chooseCabinets(choice) == std::vector<CabinetPlay>{{7, ScreenClass::Ambient}});
    // Its enabled off: no attract.
    choice.attract = false;
    CHECK(chooseCabinets(choice).empty());
    choice.attract = true;
    choice.attractPick.reset();
    CHECK(chooseCabinets(choice).empty());
}

TEST_CASE("app/cabinets: no cabinet plays while the room editor runs", "[app][screens][cabinets][edit]") {
    // ADR-0015 §4's session moves, holds and rebuilds the cabinets: every one shows its cover until it ends.
    CabinetChoice choice;
    choice.attractPick = 7;
    choice.editing = true;
    CHECK(chooseCabinets(choice).empty());
    choice.state = WorldState::Edit;
    choice.editing = false;
    CHECK(chooseCabinets(choice).empty());
    choice.state = WorldState::Inspect;
    choice.focused = 3;
    choice.editing = true;
    CHECK(chooseCabinets(choice).empty());
    // After the session: the focused cabinet and attract again.
    choice.editing = false;
    CHECK(chooseCabinets(choice) == std::vector<CabinetPlay>{{3, ScreenClass::Direct}});
}

TEST_CASE("app/cabinets: a cabinet's screen id names its room and its item", "[app][screens][cabinets]") {
    CHECK(rv::app::cabinetScreenId("area:arcade", 0x00A1B2C3u) == "area:arcade/cabinet:00A1B2C3");
}
