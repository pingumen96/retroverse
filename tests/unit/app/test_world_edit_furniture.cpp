// The editor's furniture (I4b step 4; ADR-0015 §4): the edited room's movable fixtures as targets where no game or slot
// is, the floor and wall points a held fixture follows, the move it is drawn with; the interaction picking a fixture up
// (Moving), turning it (a quarter turn, decoration 15 degrees, a poster never), nudging it on the room's grid along the
// view's axes (a poster along its wall), following the reticle from where it was taken, judging each new place (a
// probe, its verdict), dropping it (an operation, kept in the hand when refused), putting it back; a fixture of a kept
// room refused at once; drops refused through the draft as the editor makes them (snapped by world::snapFixture()): in
// a doorway, in a door-reserved socket, over another fixture, closing the 1.2 m passage, cutting a module off; a room's
// static part rebuilt alone equal to the floor's scene; a held game's ray passing the dressing.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/app/world_build.hpp>
#include <rv/app/world_edit_draft.hpp>
#include <rv/app/world_edit_interaction.hpp>
#include <rv/app/world_edit_room.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_edit.hpp>

#include "../world/plan_scene_support.hpp"
#include "../world/world_generation_support.hpp"

namespace {

using rv::app::EditDraft;
using rv::app::EditFrame;
using rv::app::EditFrameResult;
using rv::app::EditInteraction;
using rv::app::EditMode;
using rv::app::EditRoom;
using rv::input::Action;
using rv::input::ActionState;
using rv::world::EditedWorld;
using rv::world::EditOperation;
using rv::world::Fixture;
using rv::world::FixtureId;
using rv::world::FixtureRole;
using rv::world::FixtureTransform;
using rv::world::WorldPlan;

constexpr std::uint64_t kWorldSeed = 102938;
const rv::world::PinProvenance kProvenance{rv::world::PinOrigin::Editor, "2026-10-28T12:00:00.000Z"};

/// A fresh SNES room of 40 titles (the world tests' fixture room): its display cases, side modules and decoration.
const rv::world::GenerationResult& room40() {
    static const rv::world::GenerationResult result = rv::test::world::generateRoom(40, 3, kWorldSeed);
    return result;
}

std::shared_ptr<const rv::world::WorldData> unowned(const rv::world::WorldData& data) {
    return std::shared_ptr<const rv::world::WorldData>(std::shared_ptr<const rv::world::WorldData>(), &data);
}

std::shared_ptr<const rv::world::EditData> snesEditData() {
    auto data = std::make_shared<rv::world::EditData>();
    data->config = rv::test::world::roomConfig(kWorldSeed);
    data->building = rv::test::world::snesData().building;
    data->rooms.emplace(room40().plan.rooms.front().id, unowned(rv::test::world::snesData()));
    return data;
}

const rv::world::Room& roomOf(const WorldPlan& plan) {
    return plan.rooms.front();
}

/// The room's frame as the editor gets it: the scene's frame is the room's (a start room).
rv::app::EditRoomFrame frameOf(const WorldPlan& plan) {
    const rv::world::Room& room = roomOf(plan);
    rv::app::EditRoomFrame frame;
    frame.minXMm = room.minXMm;
    frame.maxXMm = room.maxXMm;
    frame.minZMm = room.minZMm;
    frame.maxZMm = room.maxZMm;
    frame.heightMm = room.heightMm;
    return frame;
}

rv::world::CollectionLayout layoutOf(const WorldPlan& plan) {
    auto data = rv::world::loadSceneData(rv::test::world::sourceAssetRoot(), plan, roomOf(plan).id);
    REQUIRE(data.has_value());
    auto layout = rv::world::collectionLayout(plan, roomOf(plan).id, *data, rv::test::world::inventedTitles(plan));
    REQUIRE(layout.has_value());
    return std::move(*layout);
}

std::unique_ptr<EditRoom> editRoomOf(const WorldPlan& plan, const rv::world::EditData& data) {
    auto room = EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(layoutOf(plan)),
                                rv::world::editableFixtures(plan, roomOf(plan).id, data), frameOf(plan));
    REQUIRE(room.has_value());
    return std::move(*room);
}

const Fixture& fixtureOf(const WorldPlan& plan, std::string_view id) {
    const auto found = std::find_if(plan.fixtures.begin(), plan.fixtures.end(),
                                    [&](const Fixture& fixture) { return fixture.id.toString() == id; });
    REQUIRE(found != plan.fixtures.end());
    return *found;
}

template <class Accept>
const Fixture* findFixture(const WorldPlan& plan, Accept&& accept) {
    for (const Fixture& fixture : plan.fixtures) {
        if (accept(fixture)) {
            return &fixture;
        }
    }
    return nullptr;
}

FixtureTransform placeOf(const Fixture& fixture) {
    return FixtureTransform{fixture.xMm, fixture.zMm, fixture.yawCdeg};
}

ActionState press(std::initializer_list<Action> actions) {
    ActionState state;
    for (const Action action : actions) {
        state.setButton(action, true, true, false);
    }
    return state;
}

/// One frame of the editor: `actions` pressed (none: nothing), the reticle on `fixture` where no game or slot is, the
/// floor and wall points under it, the view facing `forward` (room's frame x, z).
struct Aim {
    std::optional<FixtureId> fixture;
    std::optional<FixtureTransform> floor;
    std::optional<FixtureTransform> wall;
    glm::vec2 forward{0.0f, -1.0f};
    bool looked = false;
};

EditFrameResult frame(EditInteraction& editor, std::initializer_list<Action> actions, const Aim& aim) {
    ActionState state = press(actions);
    if (aim.looked) {
        state.setAxis(Action::LookDeltaX, 3.0f);
    }
    EditFrame input;
    input.actions = &state;
    input.deltaSeconds = 0.016667f;
    input.hover = rv::world::HoverSample{};
    input.fixture = aim.fixture;
    input.floorAim = aim.floor;
    input.wallAim = aim.wall;
    input.roomForward = aim.forward;
    return editor.update(input);
}

/// The editor ready in `room`, its fixtures snapped by the draft's world.
EditInteraction readyEditor(const EditRoom& room, const EditedWorld& world, const rv::world::EditData& data) {
    EditInteraction editor({}, true);
    editor.begin(std::nullopt);
    editor.setRoom(&room);
    editor.ready();
    editor.setPlacer([&world, &data](const FixtureId& id, const FixtureTransform& wanted) {
        return rv::world::snapFixture(world, id, wanted, data);
    });
    return editor;
}

/// The refusal's rule of `operation` applied through a draft of `world` ("" applied).
std::string dropped(const EditedWorld& world, const std::shared_ptr<const rv::world::EditData>& data,
                    const EditOperation& operation, std::string* subject = nullptr) {
    EditDraft draft(rv::app::EditBase{world, "base"}, data);
    auto result = draft.apply(operation, kProvenance);
    REQUIRE(result.has_value());
    if (result->refused && subject != nullptr) {
        *subject = result->refused->subject;
    }
    return result->refused ? result->refused->rule : std::string();
}

/// A move of `fixture` to the place `wanted` snaps to on its grid (the editor's drop).
EditOperation snappedMove(const EditedWorld& world, const rv::world::EditData& data, const Fixture& fixture,
                          const FixtureTransform& wanted) {
    const std::optional<FixtureTransform> place = rv::world::snapFixture(world, fixture.id, wanted, data);
    REQUIRE(place.has_value());
    return EditOperation::moveFixture(fixture.id, place->xMm, place->zMm, place->yawCdeg);
}

} // namespace

TEST_CASE("app/world_edit_furniture: the room's movable fixtures are targets, the floor and the walls are aimed at",
          "[app][world_edit]") {
    const WorldPlan& plan = room40().plan;
    const auto data = snesEditData();
    const std::unique_ptr<EditRoom> room = editRoomOf(plan, *data);
    // Side-wall modules, display fixtures and decoration; never the hero corner nor the hero-wall modules.
    REQUIRE_FALSE(room->fixtures().empty());
    for (const rv::world::EditableFixture& fixture : room->fixtures()) {
        INFO(fixture.id.toString());
        CHECK((fixture.role == FixtureRole::SideModule || fixture.role == FixtureRole::Display ||
               fixture.role == FixtureRole::Decoration));
        CHECK(fixture.refusal.empty());
        const Fixture& planned = fixtureOf(plan, fixture.id.toString());
        CHECK(fixture.at == placeOf(planned));
        CHECK(fixture.box == rv::world::fixtureBox(planned));
        CHECK(fixture.hung == (planned.role == FixtureRole::Decoration && planned.yMm > 0));
        const auto target = room->fixtureTarget(fixture.id);
        REQUIRE(target.has_value());
        CHECK(target->fixture == fixture.id);
        CHECK_FALSE(target->free());
    }
    CHECK(std::none_of(room->fixtures().begin(), room->fixtures().end(), [](const auto& fixture) {
        return fixture.role == FixtureRole::HeroCorner || fixture.role == FixtureRole::HeroModule;
    }));
    // A ray at a display case's box from 2 m in front of it, at its middle's height, reaches it.
    const Fixture& display = fixtureOf(plan, "room:snes/fx:dc01");
    const rv::world::FixtureBox box = rv::world::fixtureBox(display);
    const glm::vec3 middle(static_cast<float>(box.minXMm + box.maxXMm) / 2000.0f,
                           static_cast<float>(box.minYMm + box.maxYMm) / 2000.0f,
                           static_cast<float>(box.minZMm + box.maxZMm) / 2000.0f);
    const float front = static_cast<float>(box.maxZMm) / 1000.0f;
    const rv::core::Ray ray{glm::vec3(middle.x, middle.y, front + 2.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    CHECK(room->fixtureAt(ray) == display.id);
    CHECK_FALSE(room->fixtureAt(ray, 1.5f).has_value());
    CHECK_FALSE(room->fixtureAt(rv::core::Ray{ray.origin, glm::vec3(0.0f, 0.0f, 1.0f)}).has_value());
    // The floor under a ray looking down, kept within the room; nothing looking up.
    const rv::world::Room& interior = roomOf(plan);
    const auto floor = room->floorAim(rv::core::Ray{glm::vec3(0.5f, 1.6f, 0.0f), glm::vec3(0.0f, -1.0f, -1.0f)});
    REQUIRE(floor.has_value());
    CHECK(floor->xMm == 500);
    CHECK(floor->zMm == -1600);
    CHECK_FALSE(room->floorAim(rv::core::Ray{glm::vec3(0.0f, 1.6f, 0.0f), glm::vec3(0.0f, 0.2f, -1.0f)}).has_value());
    const auto far = room->floorAim(rv::core::Ray{glm::vec3(0.0f, 1.6f, 0.0f), glm::vec3(0.0f, -0.01f, -1.0f)});
    REQUIRE(far.has_value());
    CHECK(far->zMm == interior.minZMm);
    // The wall a ray from the room's middle meets: the west wall looking west, the north wall looking north.
    const auto west = room->wallAim(rv::core::Ray{glm::vec3(0.0f, 1.5f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.1f)});
    REQUIRE(west.has_value());
    CHECK(west->xMm == interior.minXMm);
    const auto north = room->wallAim(rv::core::Ray{glm::vec3(0.0f, 1.5f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)});
    REQUIRE(north.has_value());
    CHECK(north->zMm == interior.minZMm);
    CHECK(north->xMm == 0);
    // The scene's frame turned half round (a south-row room): the room's frame read back.
    rv::app::EditRoomFrame turned = frameOf(plan);
    turned.toScene = rv::world::roomToFloorMatrix(rv::world::RoomPlacement{3000, 8000, 1});
    auto turnedRoom = EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(layoutOf(plan)), {}, turned);
    REQUIRE(turnedRoom.has_value());
    const glm::vec2 direction = (*turnedRoom)->roomDirection(glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(direction.x == 0.0f);
    CHECK(direction.y == 1.0f);

    // A fixture drawn where it would stand: the move from its place to another about its pivot, in the scene's frame.
    const FixtureTransform from{1000, 2000, 0};
    const FixtureTransform to{1500, 2000, 9000};
    const glm::mat4 move = rv::app::fixtureMove(from, to, glm::mat4(1.0f));
    const glm::vec3 pivot = glm::vec3(move * glm::vec4(1.0f, 0.0f, 2.0f, 1.0f));
    CHECK(glm::length(pivot - glm::vec3(1.5f, 0.0f, 2.0f)) < 1e-5f);
    // Its front (+Z at yaw 0) faces +X after a quarter turn.
    const glm::vec3 facing = glm::vec3(move * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    CHECK(glm::length(facing - glm::vec3(1.0f, 0.0f, 0.0f)) < 1e-5f);
    const glm::mat4 turnedMove = rv::app::fixtureMove(from, to, turned.toScene);
    const glm::vec3 inScene = glm::vec3(turnedMove * turned.toScene * glm::vec4(1.0f, 0.0f, 2.0f, 1.0f));
    const glm::vec3 expected = glm::vec3(turned.toScene * glm::vec4(1.5f, 0.0f, 2.0f, 1.0f));
    CHECK(glm::length(inScene - expected) < 1e-4f);
}

TEST_CASE(
    "app/world_edit_furniture: a fixture picked up, turned, nudged on the grid, following the reticle, judged and "
    "dropped",
    "[app][world_edit]") {
    const WorldPlan& plan = room40().plan;
    const auto data = snesEditData();
    const EditedWorld world{plan, {}};
    const std::unique_ptr<EditRoom> room = editRoomOf(plan, *data);
    const Fixture& display = fixtureOf(plan, "room:snes/fx:dc01");
    const FixtureTransform at = placeOf(display);
    EditInteraction editor = readyEditor(*room, world, *data);

    // The reticle on the display case where no game is: its target, then interact picks it up (Moving), held where
    // the reticle took it (its pivot 400 mm from the floor point under the reticle).
    Aim aim;
    aim.fixture = display.id;
    aim.floor = FixtureTransform{at.xMm, at.zMm + 400, 0};
    static_cast<void>(frame(editor, {}, aim));
    REQUIRE(editor.target().has_value());
    CHECK(editor.target()->fixture == display.id);
    EditFrameResult result = frame(editor, {Action::Interact}, aim);
    REQUIRE(editor.mode() == EditMode::Moving);
    REQUIRE(editor.heldFixture().has_value());
    CHECK(editor.heldFixture()->from == at);
    CHECK(editor.heldFixture()->place == at);
    CHECK_FALSE(result.apply.has_value());
    // Where it stood it may drop (nothing to judge).
    CHECK(editor.verdict() == std::string());
    aim.fixture.reset();

    // A quarter turn left: the place judged (a probe of the move), its verdict unknown until it comes.
    result = frame(editor, {Action::EditRotateLeft}, aim);
    CHECK(editor.heldFixture()->place == FixtureTransform{at.xMm, at.zMm, (at.yawCdeg + 9000) % 36000});
    REQUIRE(result.probe.has_value());
    CHECK(*result.probe == EditOperation::moveFixture(display.id, at.xMm, at.zMm, (at.yawCdeg + 9000) % 36000));
    CHECK_FALSE(editor.verdict().has_value());
    editor.setVerdict(*result.probe, "");
    CHECK(editor.verdict() == std::string());
    // The same place is not judged again.
    CHECK_FALSE(frame(editor, {}, aim).probe.has_value());
    // Turned right twice: three quarters from where it stood.
    static_cast<void>(frame(editor, {Action::EditRotateRight}, aim));
    static_cast<void>(frame(editor, {Action::EditRotateRight}, aim));
    CHECK(editor.heldFixture()->place.yawCdeg == (at.yawCdeg + 27000) % 36000);
    static_cast<void>(frame(editor, {Action::EditRotateLeft}, aim));
    CHECK(editor.heldFixture()->place.yawCdeg == at.yawCdeg);

    // Nudged by the D-pad on the grid, along the room's axes nearest the view's: facing north (-z), right is +x.
    result = frame(editor, {Action::UiRight}, aim);
    CHECK(editor.heldFixture()->place == FixtureTransform{at.xMm + 100, at.zMm, at.yawCdeg});
    REQUIRE(result.probe.has_value());
    editor.setVerdict(*result.probe, "footprints-overlap");
    CHECK(editor.verdict() == std::string("footprints-overlap"));
    static_cast<void>(frame(editor, {Action::UiUp}, aim));
    CHECK(editor.heldFixture()->place == FixtureTransform{at.xMm + 100, at.zMm - 100, at.yawCdeg});
    // A verdict about another place says nothing of this one.
    CHECK_FALSE(editor.verdict().has_value());
    // Facing east (+x): up is +x, right +z.
    aim.forward = glm::vec2(0.9f, 0.2f);
    static_cast<void>(frame(editor, {Action::UiUp}, aim));
    static_cast<void>(frame(editor, {Action::UiRight}, aim));
    CHECK(editor.heldFixture()->place == FixtureTransform{at.xMm + 200, at.zMm, at.yawCdeg});
    // Nudged, it stays while the reticle rests; the player's look gives it back to the reticle (with the offset it
    // was taken with), on the grid through its home.
    aim.floor = FixtureTransform{at.xMm - 1234, at.zMm + 400, 0};
    static_cast<void>(frame(editor, {}, aim));
    CHECK(editor.heldFixture()->place == FixtureTransform{at.xMm + 200, at.zMm, at.yawCdeg});
    aim.looked = true;
    static_cast<void>(frame(editor, {}, aim));
    CHECK(editor.heldFixture()->place == FixtureTransform{at.xMm - 1200, at.zMm, at.yawCdeg});
    aim.looked = false;

    // Dropped: the move of the place it shows at; Busy until applied; a refusal keeps it in the hand.
    result = frame(editor, {Action::Interact}, aim);
    REQUIRE(result.apply.has_value());
    CHECK(*result.apply == EditOperation::moveFixture(display.id, at.xMm - 1200, at.zMm, at.yawCdeg));
    CHECK(editor.mode() == EditMode::Busy);
    CHECK_FALSE(frame(editor, {Action::Interact}, aim).apply.has_value());
    editor.applied(false);
    CHECK(editor.mode() == EditMode::Moving);
    REQUIRE(editor.heldFixture().has_value());
    result = frame(editor, {Action::Interact}, aim);
    REQUIRE(result.apply.has_value());
    editor.applied(true);
    CHECK(editor.mode() == EditMode::Browse);
    CHECK_FALSE(editor.heldFixture().has_value());

    // The reticle off it: nothing targeted.
    CHECK_FALSE(editor.target().has_value());
    aim.fixture = display.id;
    static_cast<void>(frame(editor, {}, aim));
    REQUIRE(editor.target().has_value());
    aim.fixture.reset();
    static_cast<void>(frame(editor, {}, aim));
    CHECK_FALSE(editor.target().has_value());

    // Back puts it back, and so does interact where it stood: no operation.
    aim.fixture = display.id;
    static_cast<void>(frame(editor, {}, aim));
    static_cast<void>(frame(editor, {Action::Interact}, aim));
    REQUIRE(editor.mode() == EditMode::Moving);
    static_cast<void>(frame(editor, {Action::UiLeft}, aim));
    result = frame(editor, {Action::Back}, aim);
    CHECK_FALSE(result.apply.has_value());
    CHECK(editor.mode() == EditMode::Browse);
    CHECK_FALSE(editor.heldFixture().has_value());
    static_cast<void>(frame(editor, {Action::Interact}, aim));
    REQUIRE(editor.mode() == EditMode::Moving);
    CHECK_FALSE(frame(editor, {Action::Interact}, aim).apply.has_value());
    CHECK(editor.mode() == EditMode::Browse);
    // The menu lets go of it.
    static_cast<void>(frame(editor, {Action::Interact}, aim));
    REQUIRE(editor.mode() == EditMode::Moving);
    static_cast<void>(frame(editor, {Action::Confirm}, aim));
    CHECK(editor.mode() == EditMode::Menu);
    CHECK_FALSE(editor.heldFixture().has_value());
    static_cast<void>(frame(editor, {Action::Back}, aim));
    CHECK(editor.mode() == EditMode::Browse);
    // edit_pin pins the fixture under the reticle in place, or unpins it.
    editor.setFixturePinned([&](const FixtureId& id) { return id == display.id; });
    static_cast<void>(frame(editor, {}, aim));
    CHECK(editor.targetPinned());
    result = frame(editor, {Action::EditPin}, aim);
    REQUIRE(result.apply.has_value());
    CHECK(*result.apply == EditOperation::unpinFixture(display.id));
}

TEST_CASE("app/world_edit_furniture: decoration turns by 15 degrees, a poster slides along its wall, a kept room's "
          "fixtures stay",
          "[app][world_edit]") {
    const WorldPlan& plan = room40().plan;
    const auto data = snesEditData();
    const EditedWorld world{plan, {}};
    const std::unique_ptr<EditRoom> room = editRoomOf(plan, *data);

    SECTION("a decoration standing on the floor") {
        const Fixture* standing = findFixture(plan, [](const Fixture& fixture) {
            return fixture.role == FixtureRole::Decoration && fixture.yMm == 0 && fixture.prefab.kind != "rug";
        });
        REQUIRE(standing != nullptr);
        EditInteraction editor = readyEditor(*room, world, *data);
        Aim aim;
        aim.fixture = standing->id;
        static_cast<void>(frame(editor, {}, aim));
        static_cast<void>(frame(editor, {Action::Interact}, aim));
        REQUIRE(editor.mode() == EditMode::Moving);
        static_cast<void>(frame(editor, {Action::EditRotateLeft}, aim));
        CHECK(editor.heldFixture()->place.yawCdeg == (standing->yawCdeg + 1500) % 36000);
    }
    SECTION("a poster: never turned by the keys, nudged along its wall only") {
        const Fixture* poster = findFixture(
            plan, [](const Fixture& fixture) { return fixture.role == FixtureRole::Decoration && fixture.yMm > 0; });
        REQUIRE(poster != nullptr);
        EditInteraction editor = readyEditor(*room, world, *data);
        Aim aim;
        aim.fixture = poster->id;
        static_cast<void>(frame(editor, {}, aim));
        static_cast<void>(frame(editor, {Action::Interact}, aim));
        REQUIRE(editor.mode() == EditMode::Moving);
        CHECK(editor.heldFixture()->hung);
        static_cast<void>(frame(editor, {Action::EditRotateLeft}, aim));
        CHECK(editor.heldFixture()->place == placeOf(*poster));
        // Facing its wall (it faces into the room: the view looks against its front).
        const float yaw = static_cast<float>(poster->yawCdeg) / 100.0f;
        aim.forward = -glm::vec2(std::sin(glm::radians(yaw)), std::cos(glm::radians(yaw)));
        const bool alongX = poster->yawCdeg % 18000 == 0;
        static_cast<void>(frame(editor, {Action::UiUp}, aim));
        CHECK(editor.heldFixture()->place == placeOf(*poster));
        for (const Action side : {Action::UiRight, Action::UiLeft}) {
            static_cast<void>(frame(editor, {side}, aim));
            const FixtureTransform& place = editor.heldFixture()->place;
            CHECK(place.yawCdeg == poster->yawCdeg);
            if (alongX) {
                CHECK(place.zMm == poster->zMm);
            } else {
                CHECK(place.xMm == poster->xMm);
            }
        }
        // Right then left: back where it hung.
        CHECK(editor.heldFixture()->place == placeOf(*poster));
        static_cast<void>(frame(editor, {Action::UiRight}, aim));
        CHECK(editor.heldFixture()->moved());
    }
    SECTION("a room kept as an older template made it: its fixtures named fixed, nothing held") {
        WorldPlan kept = plan;
        kept.rooms.front().templateVersion = 1;
        const EditedWorld keptWorld{kept, {}};
        const auto keptRoom = EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(layoutOf(plan)),
                                              rv::world::editableFixtures(kept, roomOf(kept).id, *data), frameOf(kept));
        REQUIRE(keptRoom.has_value());
        for (const rv::world::EditableFixture& fixture : (*keptRoom)->fixtures()) {
            CHECK(fixture.refusal == rv::world::edit_rule::kFixtureInKeptRoom);
        }
        EditInteraction editor = readyEditor(**keptRoom, keptWorld, *data);
        Aim aim;
        aim.fixture = (*keptRoom)->fixtures().front().id;
        static_cast<void>(frame(editor, {}, aim));
        const EditFrameResult result = frame(editor, {Action::Interact}, aim);
        CHECK(result.refused == rv::world::edit_rule::kFixtureInKeptRoom);
        CHECK(editor.mode() == EditMode::Browse);
        CHECK_FALSE(editor.heldFixture().has_value());
    }
}

TEST_CASE("app/world_edit_furniture: drops refused in a doorway, a door-reserved socket, over another fixture, closing "
          "the 1.2 m passage, cutting a module off",
          "[app][world_edit]") {
    const WorldPlan& plan = room40().plan;
    const auto data = snesEditData();
    const EditedWorld world{plan, {}};
    const rv::world::Room& room = roomOf(plan);
    const Fixture& display = fixtureOf(plan, "room:snes/fx:dc01");
    const FixtureTransform at = placeOf(display);

    SECTION("in the doorway's clear area") {
        const std::int64_t toDoorway = ((room.maxZMm - 700 - display.zMm) / 100) * 100;
        CHECK(dropped(world, data, snappedMove(world, *data, display, {at.xMm + 1000, at.zMm + toDoorway, 0})) ==
              rv::world::rule::kDoorwayClearance);
    }
    SECTION("in a door-reserved socket") {
        // Along the west wall in front of its door-reserved socket (x -4470 to -4070, z -400 to 1400).
        CHECK(dropped(world, data, snappedMove(world, *data, display, {at.xMm - 3200, at.zMm - 500, at.yawCdeg})) ==
              rv::world::rule::kDoorReserved);
    }
    SECTION("over another fixture") {
        const Fixture* other = findFixture(plan, [&](const Fixture& fixture) {
            return fixture.role == FixtureRole::Display && fixture.id != display.id;
        });
        REQUIRE(other != nullptr);
        CHECK(dropped(world, data, snappedMove(world, *data, display, placeOf(*other))) ==
              rv::world::rule::kFootprintsOverlap);
    }
    SECTION("cutting a module off: a display case right in front of a module") {
        bool found = false;
        std::string seen;
        for (const Fixture& module : plan.fixtures) {
            if ((module.role != FixtureRole::SideModule && module.role != FixtureRole::HeroModule) || found) {
                continue;
            }
            // In front of it, facing the same way, a step out at a time.
            const float yaw = glm::radians(static_cast<float>(module.yawCdeg) / 100.0f);
            const glm::vec2 front(std::sin(yaw), std::cos(yaw));
            for (std::int64_t out = 300; out <= 1200 && !found; out += 100) {
                const std::int64_t reach = (module.depthMm + display.depthMm) / 2 + out;
                const FixtureTransform wanted{module.xMm + std::llround(front.x * static_cast<float>(reach)),
                                              module.zMm + std::llround(front.y * static_cast<float>(reach)),
                                              module.yawCdeg};
                std::string subject;
                const std::string rule = dropped(world, data, snappedMove(world, *data, display, wanted), &subject);
                seen += module.id.toString() + " +" + std::to_string(out) + ": " + rule + " " + subject + "\n";
                found = rule == rv::world::rule::kUnreachable && subject == module.id.toString();
            }
        }
        INFO(seen);
        CHECK(found);
    }
    SECTION("closing the 1.2 m passage: something across the room no longer reached") {
        // The display case across the room's middle, turned or not, on its grid: a place refused because a target
        // that does not stand right against it (more than 1.2 m from the case) is no longer reached.
        bool found = false;
        std::string seen;
        for (const std::int32_t yaw : {0, 9000}) {
            for (std::int64_t z = room.minZMm + 1000; z <= room.maxZMm - 1000 && !found; z += 200) {
                for (std::int64_t x = room.minXMm + 600; x <= room.maxXMm - 600 && !found; x += 200) {
                    std::string subject;
                    const EditOperation move = snappedMove(world, *data, display, {x, z, yaw});
                    if (dropped(world, data, move, &subject) != rv::world::rule::kUnreachable ||
                        subject == display.id.toString()) {
                        continue;
                    }
                    // The target cut off stands away from the case: the passage, not its front, was closed.
                    Fixture placed = display;
                    placed.xMm = move.xMm;
                    placed.zMm = move.zMm;
                    placed.yawCdeg = *move.yawCdeg;
                    const rv::world::FixtureBox caseBox = rv::world::fixtureBox(placed);
                    const rv::world::FixtureBox targetBox = rv::world::fixtureBox(fixtureOf(plan, subject));
                    const std::int64_t gapX = std::max(
                        {caseBox.minXMm - targetBox.maxXMm, targetBox.minXMm - caseBox.maxXMm, std::int64_t{0}});
                    const std::int64_t gapZ = std::max(
                        {caseBox.minZMm - targetBox.maxZMm, targetBox.minZMm - caseBox.maxZMm, std::int64_t{0}});
                    seen += "x " + std::to_string(move.xMm) + " z " + std::to_string(move.zMm) + ": " + subject +
                            " gap " + std::to_string(std::max(gapX, gapZ)) + "\n";
                    found = std::max(gapX, gapZ) > 1200;
                }
            }
        }
        INFO(seen);
        CHECK(found);
    }
    // Every refusal leaves the draft as it was; an accepted drop is kept and pinned where it was put.
    EditDraft draft(rv::app::EditBase{world, "base"}, data);
    auto accepted = draft.apply(snappedMove(world, *data, display, {at.xMm, at.zMm + 300, at.yawCdeg}), kProvenance);
    REQUIRE(accepted.has_value());
    REQUIRE_FALSE(accepted->refused);
    CHECK(draft.size() == 1);
    CHECK(accepted->rebuild == rv::world::RebuildLevel::LocalReflow);
    REQUIRE(draft.current().overrides.fixturePins.size() == 1);
    CHECK(draft.current().overrides.fixturePins.front().fixture == display.id);
}

TEST_CASE("app/world_edit_furniture: a room's static part rebuilt alone is the floor's scene of that room",
          "[app][world_edit]") {
    const WorldPlan& plan = room40().plan;
    const auto data = snesEditData();
    const rv::world::RoomId roomId = roomOf(plan).id;
    auto sceneData = rv::world::loadSceneData(rv::test::world::sourceAssetRoot(), plan, roomId);
    REQUIRE(sceneData.has_value());

    // The floor shown: the room on its floor (its own frame the scene's: the start room, as buildFloor() frames it).
    rv::app::WorldBuild shown;
    shown.startRoom = roomId;
    shown.plan = plan;
    shown.floor = roomOf(plan).floor;
    shown.floorPlacement = rv::app::relativePlacement(rv::world::RoomPlacement{}, roomOf(plan).placement);
    rv::app::RoomCollection collection;
    collection.room = roomId;
    collection.placement = roomOf(plan).placement;
    collection.sceneData = *sceneData;
    shown.rooms.push_back(collection);

    // A display case moved, the floor's scene of the edited plan: the room's objects and lights are the part's.
    EditedWorld world{plan, {}};
    const Fixture& display = fixtureOf(plan, "room:snes/fx:dc01");
    auto moved = rv::world::applyEdit(
        world, snappedMove(world, *data, display, {display.xMm, display.zMm + 300, display.yawCdeg}), *data,
        kProvenance);
    REQUIRE(moved.has_value());
    REQUIRE_FALSE(moved->refused);
    auto part = rv::app::roomStaticPart(shown, world.plan, roomId);
    REQUIRE(part.has_value());
    rv::world::FloorSceneData floorData;
    floorData.building = rv::test::world::snesData().building;
    floorData.rooms.emplace(roomId, *sceneData);
    floorData.lighting = sceneData->lighting;
    auto floorScene = rv::world::toFloorSceneFile(world.plan, roomOf(plan).floor, floorData);
    REQUIRE(floorScene.has_value());
    rv::app::moveSceneFileToRoom(floorScene->sceneFile, roomOf(plan).placement);
    auto floor = rv::scene::parseSceneDescription(floorScene->sceneFile, "floor");
    REQUIRE(floor.has_value());
    const std::string prefix = roomId.toString() + "/";
    std::vector<rv::scene::SceneObject> objects;
    for (const rv::scene::SceneObject& object : floor->objects) {
        if (object.id.starts_with(prefix)) {
            objects.push_back(object);
        }
    }
    REQUIRE(objects.size() == part->objects.size());
    CHECK(objects == part->objects);
    std::size_t lights = 0;
    for (const rv::scene::Light& light : floor->lights) {
        lights += light.name.starts_with(prefix) ? 1 : 0;
    }
    CHECK(lights == part->lights.size());
    // The case's own object moved with it; the room's shell stands as it was.
    const auto caseObject = std::find_if(part->objects.begin(), part->objects.end(),
                                         [&](const auto& object) { return object.id == prefix + "fx:dc01"; });
    REQUIRE(caseObject != part->objects.end());
    auto before = rv::app::roomStaticPart(shown, plan, roomId);
    REQUIRE(before.has_value());
    const auto caseBefore = std::find_if(before->objects.begin(), before->objects.end(),
                                         [&](const auto& object) { return object.id == prefix + "fx:dc01"; });
    REQUIRE(caseBefore != before->objects.end());
    CHECK_FALSE(*caseObject == *caseBefore);
    CHECK(before->objects.front() == part->objects.front());
    // The scene's frame is the floor shown's (its floorPlacement, as buildFloor() framed it): a start room turned half
    // round elsewhere on the floor moves the part as it moved the floor.
    const rv::world::RoomPlacement start{3000, 8000, 1};
    rv::app::WorldBuild turned = shown;
    turned.floorPlacement = rv::app::relativePlacement(rv::world::RoomPlacement{}, start);
    auto turnedPart = rv::app::roomStaticPart(turned, world.plan, roomId);
    REQUIRE(turnedPart.has_value());
    auto turnedScene = rv::world::toFloorSceneFile(world.plan, roomOf(plan).floor, floorData);
    REQUIRE(turnedScene.has_value());
    rv::app::moveSceneFileToRoom(turnedScene->sceneFile, start);
    auto turnedFloor = rv::scene::parseSceneDescription(turnedScene->sceneFile, "floor");
    REQUIRE(turnedFloor.has_value());
    std::vector<rv::scene::SceneObject> turnedObjects;
    for (const rv::scene::SceneObject& object : turnedFloor->objects) {
        if (object.id.starts_with(prefix)) {
            turnedObjects.push_back(object);
        }
    }
    CHECK(turnedObjects == turnedPart->objects);
    CHECK_FALSE(turnedObjects == part->objects);
    // A room the floor shown keeps no data of cannot be rebuilt alone.
    shown.rooms.front().sceneData = rv::world::SceneData{};
    CHECK_FALSE(rv::app::roomStaticPart(shown, plan, roomId).has_value());
}

TEST_CASE("app/world_edit_furniture: a held game's ray passes the dressing that gives way", "[app][world_edit]") {
    // The golden SNES room of 15 titles: dressed levels whose props occlude their free slots.
    const rv::test::world::PlanCase& entry = rv::test::world::planCase("plan-15-seed-102938");
    const rv::world::CollectionLayout layout = rv::test::world::layoutOf(entry);
    REQUIRE(layout.dressingOccluders > 0);
    REQUIRE(layout.dressingOccluders <= layout.occluders.size());
    auto room = EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(layout));
    REQUIRE(room.has_value());
    // A dressed free slot seen through its prop from in front of it: hidden from the browsing reticle, a target for the
    // held game's.
    bool found = false;
    for (const rv::world::FreeSlot& free : layout.freeSlots) {
        if (!free.dressed) {
            continue;
        }
        const glm::vec3 centre = free.worldBounds().center();
        const glm::vec3 out = glm::normalize(glm::vec3(free.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)));
        const rv::core::Ray ray{centre + 1.5f * out, -out};
        const rv::world::HoverSample browsing = (*room)->index().sample(ray, rv::world::kNoPick);
        const rv::world::HoverSample holding = (*room)->heldIndex().sample(ray, rv::world::kNoPick);
        if (browsing.hit != free.pickId && holding.hit == free.pickId) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("app/world_edit_furniture: a held game's move or swap is judged ahead, once per slot", "[app][world_edit]") {
    const WorldPlan& plan = room40().plan;
    const auto data = snesEditData();
    const EditedWorld world{plan, {}};
    const std::unique_ptr<EditRoom> room = editRoomOf(plan, *data);
    // A game with a free slot beside it, and another game.
    std::optional<std::pair<rv::world::PickId, rv::world::PickId>> pair;
    for (const rv::world::CollectionItem& item : room->layout().items) {
        const auto next = room->step(item.pickId, rv::world::StepDirection::Right);
        if (next && room->freeSlot(*next) != nullptr) {
            pair = std::pair(item.pickId, *next);
            break;
        }
    }
    REQUIRE(pair.has_value());
    const auto [item, free] = *pair;
    const rv::catalog::GameId game = *room->target(item)->game;
    const rv::world::CollectionItem* other = nullptr;
    for (const rv::world::CollectionItem& candidate : room->layout().items) {
        if (candidate.pickId != item) {
            other = &candidate;
            break;
        }
    }
    REQUIRE(other != nullptr);
    EditInteraction editor = readyEditor(*room, world, *data);
    const auto on = [&](std::initializer_list<Action> actions, rv::world::PickId pick) {
        ActionState state = press(actions);
        EditFrame input;
        input.actions = &state;
        input.deltaSeconds = 0.016667f;
        input.hover = rv::world::HoverSample{pick, pick != rv::world::kNoPick, editor.hovered() == pick};
        return editor.update(input);
    };
    for (int i = 0; i <= rv::world::kHoverChangeFrames; ++i) {
        static_cast<void>(on({}, item));
    }
    static_cast<void>(on({Action::Interact}, item));
    REQUIRE(editor.mode() == EditMode::Holding);
    // Over its own slot: nothing to judge. Over the free slot: its move, once.
    std::optional<EditOperation> probe;
    for (int i = 0; i <= rv::world::kHoverChangeFrames + 2; ++i) {
        const EditFrameResult result = on({}, free);
        if (result.probe) {
            CHECK_FALSE(probe.has_value());
            probe = result.probe;
        }
    }
    REQUIRE(probe.has_value());
    CHECK(*probe == EditOperation::move(game, room->target(free)->slot));
    // Over another game: the swap.
    probe.reset();
    for (int i = 0; i <= rv::world::kHoverChangeFrames + 2; ++i) {
        if (const EditFrameResult result = on({}, other->pickId); result.probe) {
            probe = result.probe;
        }
    }
    REQUIRE(probe.has_value());
    CHECK(*probe == EditOperation::swap(game, other->game));
}
