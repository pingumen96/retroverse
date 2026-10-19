// The room's side of the hover, the pull-out and Focus (I3 step 13a; rv/app/world_focus.hpp), GPU-free, on the golden
// plans' collection layouts (tests/fixtures/world/golden, as step 10a lays them out): a pulled item never passes
// through its neighbours or the boards it stands on and ends as ADR-0012 §3 says (70% of its depth out, its cover to
// the viewer; a face-out item tipped towards the viewer); the reticle at every item's Focus pose rests on that item,
// the walker there outside every collider; the layout as WorldInteraction's room (items, sections, D-pad steps); the
// D-pad's repeat; the pick ray from the reticle and the cursor; the label's anchor; and the wiring as the app drives
// it, frame by frame on the 50-title room (snes-room's size): the hover within 6 frames, the pull-out after the dwell
// and back, a face-out item inspected and Back to Focus, steps along the band, across into the wall shelf and between
// its ledges, held steps repeating, a still stick held 60 frames, Back to FreeRoam with every item back in its slot,
// and every transition instant under reduceMotion.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/world_focus.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/math.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/scene_geometry.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/scene_builder.hpp>

#include "../world/plan_scene_support.hpp"

using rv::app::CollectionRoom;
using rv::app::FocusStepRepeat;
using rv::app::FocusViewer;
using rv::input::Action;
using rv::world::CollectionItem;
using rv::world::CollectionLayout;
using rv::world::ItemStance;
using rv::world::PickId;
using rv::world::kNoPick;

namespace {

constexpr float kFrame = 1.0f / 60.0f;
constexpr float kEyeHeight = 1.65f;

const std::vector<std::string> kRooms{"plan-50-seed-102938", "plan-890-seed-102938",
                                      "plan-2000-seed-18446744073709551615"};

[[nodiscard]] rv::input::ActionState pressing(std::initializer_list<Action> actions) {
    rv::input::ActionState state;
    for (const Action action : actions) {
        state.setButton(action, true, true, false);
    }
    return state;
}

[[nodiscard]] rv::input::ActionState holding(std::initializer_list<Action> actions) {
    rv::input::ActionState state;
    for (const Action action : actions) {
        state.setButton(action, true, false, false);
    }
    return state;
}

/// The camera of a walker standing at `pose`.
[[nodiscard]] rv::scene::Camera cameraAt(const rv::scene::WalkerPose& pose) {
    rv::scene::Camera camera;
    camera.setPose(pose.feet + glm::vec3(0.0f, kEyeHeight, 0.0f), pose.yaw, pose.pitch);
    return camera;
}

/// The room's collision world from its real prefab geometry, as retroverse.exe builds it.
[[nodiscard]] rv::scene::CollisionWorld collisionOf(const rv::test::world::PlanCase& entry) {
    auto scene = rv::world::toSceneDescription(entry.plan, entry.room, entry.data);
    REQUIRE(scene.has_value());
    auto materials = rv::assets::MaterialLibrary::loadFromFile(rv::test::world::sourceAssetRoot() / "materials" /
                                                               "base" / "materials.json");
    REQUIRE(materials.has_value());
    return rv::scene::buildCollisionWorld(rv::scene::buildSceneGeometry(*scene, *materials));
}

/// The item's centre and its front's direction (the item prefab's +Z) at `progress` of its pull, in the room.
struct PulledPose {
    glm::vec3 centre{0.0f};
    glm::vec3 front{0.0f};
};

[[nodiscard]] PulledPose pulledPose(const CollectionItem& item, float progress, float reach) {
    const glm::mat4 transform = rv::app::pulledItemTransform(item, progress, reach);
    return PulledPose{glm::vec3(transform * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
                      glm::normalize(glm::vec3(transform * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)))};
}

[[nodiscard]] glm::vec3 slotAxis(const CollectionItem& item, int axis) {
    return glm::normalize(glm::vec3(item.slotFrame[axis]));
}

} // namespace

TEST_CASE("app/world_focus: a pulled item ends as ADR-0012 says and never passes through its neighbours",
          "[app][world_focus]") {
    for (const std::string& name : kRooms) {
        INFO(name);
        const CollectionLayout layout = rv::test::world::layoutOf(rv::test::world::planCase(name));
        auto room = CollectionRoom::build(std::make_shared<const CollectionLayout>(layout));
        REQUIRE(room.has_value());
        std::size_t spineOut = 0;
        std::size_t faceOut = 0;
        std::size_t fartherOut = 0;
        float farthestShare = 0.0f;
        for (std::size_t i = 0; i < layout.items.size(); ++i) {
            const CollectionItem& item = layout.items[i];
            INFO(item.slot.toString());
            const float reach = (*room)->reachOf(item.pickId);
            CHECK(reach == rv::app::rowReach(item, layout));
            CHECK(reach >= 0.0f);
            CHECK(reach <= rv::app::kMaxRowReach);
            // In its slot, it is the static item exactly.
            CHECK(rv::app::pulledItemTransform(item, 0.0f, reach) == item.transform);
            const PulledPose rest = pulledPose(item, 0.0f, reach);
            const PulledPose out = pulledPose(item, 1.0f, reach);
            const glm::vec3 slotOut = slotAxis(item, 2);
            if (item.stance == ItemStance::SpineOut) {
                ++spineOut;
                // 70% of its depth along the slot's +Z, farther only where its turned footprint would reach the row
                // (a side panel at the end of a row), its front (the cover) turned to the viewer.
                const float depth = item.slotBox.size().z;
                const glm::vec3 half = item.slotBox.halfExtents();
                const float expected = std::max(0.7f * depth, half.z + reach + half.x + rv::app::kPullClearance);
                fartherOut += expected > 0.7f * depth ? 1 : 0;
                farthestShare = std::max(farthestShare, expected / depth);
                CHECK(std::fabs(glm::dot(out.centre - rest.centre, slotOut) - expected) < 1e-4f);
                CHECK(glm::dot(out.front, slotOut) > 0.9999f);
                CHECK(glm::dot(rest.front, slotAxis(item, 0)) > 0.9999f);
            } else {
                ++faceOut;
                CHECK(reach == 0.0f);
                // Tipped by 10 degrees about its bottom front edge, its top towards the viewer.
                CHECK(std::fabs(glm::dot(out.front, rest.front) - std::cos(rv::core::degreesToRadians(10.0f))) < 1e-4f);
                CHECK(glm::dot(out.front - rest.front, slotAxis(item, 1)) < 0.0f);
            }
            // Never through a neighbour, never deeper into a board than standing on it, and no jump (a fiftieth of the
            // pull moves it less than a centimetre).
            glm::vec3 previous = rest.centre;
            const float allowed = item.stance == ItemStance::FaceOut ? 1.7e-3f : 5e-4f;
            for (int step = 1; step <= 50; ++step) {
                const float progress = static_cast<float>(step) / 50.0f;
                // The slot frame the pull moved: the pulled pose back from the item's space to the slot's.
                const glm::mat4 frame =
                    rv::app::pulledItemTransform(item, progress, reach) * glm::inverse(item.transform) * item.slotFrame;
                const auto box = rv::test::world::orientedBox(frame, item.slotBox);
                const rv::core::Aabb bounds = rv::app::itemBounds(item, progress, reach).inflated(0.002f);
                for (std::size_t j = 0; j < layout.items.size(); ++j) {
                    if (j == i || !bounds.intersects(layout.items[j].worldBounds())) {
                        continue;
                    }
                    const CollectionItem& other = layout.items[j];
                    const float depth =
                        rv::test::world::overlapDepth(box,
                                                      rv::test::world::orientedBox(other.slotFrame, other.slotBox));
                    INFO("progress " << progress << " against " << other.slot.toString() << ": " << depth * 1000.0f
                                     << " mm deep");
                    CHECK(depth <= 1e-4f);
                }
                for (const rv::core::Aabb& occluder : layout.occluders) {
                    if (!bounds.intersects(occluder)) {
                        continue;
                    }
                    const float depth = rv::test::world::overlapDepth(box, rv::test::world::orientedBox(occluder));
                    INFO("progress " << progress << " against an occluder: " << depth * 1000.0f << " mm deep");
                    CHECK(depth <= allowed);
                }
                const glm::vec3 centre = pulledPose(item, progress, reach).centre;
                CHECK(glm::length(centre - previous) < 0.01f);
                previous = centre;
            }
        }
        CHECK(faceOut > 0);
        if (name != "plan-50-seed-102938") {
            CHECK(spineOut > 0);
        }
        // Most spine-out items settle at 70%: only those at the ends of their rows go farther.
        INFO(fartherOut << " of " << spineOut << " spine-out items settle farther out, at most "
                        << farthestShare * 100.0f
                        << "% of their depth");
        CHECK(fartherOut * 4 <= spineOut);
        CHECK(farthestShare <= 0.85f);
    }
}

TEST_CASE("app/world_focus: the reticle at an item's Focus pose rests on that item, the walker clear of colliders",
          "[app][world_focus]") {
    for (const std::string& name : kRooms) {
        INFO(name);
        const rv::test::world::PlanCase& entry = rv::test::world::planCase(name);
        const CollectionLayout layout = rv::test::world::layoutOf(entry);
        auto index = rv::world::PickIndex::build(layout.pickTargets(), layout.occluders);
        REQUIRE(index.has_value());
        const rv::scene::CollisionWorld collision = collisionOf(entry);
        const rv::scene::Capsule capsule = rv::scene::FirstPersonSettings{}.capsule;
        FocusViewer viewer;
        std::size_t picked = 0;
        for (const CollectionItem& item : layout.items) {
            INFO(item.slot.toString());
            const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
            const rv::app::ItemFace face = rv::app::itemFace(item);
            const glm::vec3 eye = pose.feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
            CHECK(pose.feet.y == 0.0f);
            CHECK(std::fabs(glm::length(glm::vec2(eye.x - face.center.x, eye.z - face.center.z)) - 0.9f) < 1e-4f);
            const rv::world::PickHit hit = index->pick(cameraAt(pose).rayFromNdc(glm::vec2(0.0f)));
            CHECK(hit.id == item.pickId);
            picked += hit.id == item.pickId ? 1 : 0;
            CHECK_FALSE(collision.overlaps(pose.feet, capsule));
        }
        CHECK(picked == layout.items.size());
    }
}

TEST_CASE("app/world_focus: the layout as the interaction's room", "[app][world_focus]") {
    const CollectionLayout layout = rv::test::world::layoutOf(rv::test::world::planCase("plan-890-seed-102938"));
    auto built = CollectionRoom::build(std::make_shared<const CollectionLayout>(layout));
    REQUIRE(built.has_value());
    CollectionRoom& room = **built;
    auto navigation = rv::world::SlotNavigation::build(layout.navigation);
    REQUIRE(navigation.has_value());
    for (const CollectionItem& item : layout.items) {
        const auto found = room.item(item.pickId);
        REQUIRE(found.has_value());
        CHECK(found->game == item.game);
        CHECK(found->faceOut == (item.stance == ItemStance::FaceOut));
        // Its section is its fixture: every item of a fixture shares it, items of two fixtures do not.
        CHECK(found->section == (item.pickId >> 24));
        CHECK(room.find(item.pickId) != nullptr);
        for (const rv::world::StepDirection direction :
             {rv::world::StepDirection::Left, rv::world::StepDirection::Right, rv::world::StepDirection::Up,
              rv::world::StepDirection::Down}) {
            CHECK(room.step(item.pickId, direction) == navigation->step(item.pickId, direction));
        }
    }
    for (std::size_t i = 1; i < layout.items.size(); ++i) {
        const CollectionItem& a = layout.items[i - 1];
        const CollectionItem& b = layout.items[i];
        CHECK((CollectionRoom::sectionOf(a.pickId) == CollectionRoom::sectionOf(b.pickId)) ==
              (a.slot.fixture() == b.slot.fixture()));
    }
    CHECK_FALSE(room.item(kNoPick).has_value());
    CHECK_FALSE(room.item(0x7F000001u).has_value());
    // The Focus pose follows the viewer it was given.
    const CollectionItem& first = layout.items.front();
    FocusViewer raised;
    raised.floorY = 0.25f;
    room.setViewer(raised);
    CHECK(room.focusPose(first.pickId).feet.y == 0.25f);
    CHECK(room.focusPose(first.pickId).feet == rv::app::focusPoseFor(first, raised).feet);
}

TEST_CASE("app/world_focus: a held D-pad direction repeats its step in Focus", "[app][world_focus]") {
    FocusStepRepeat repeat;
    const auto steps = [&](const rv::input::ActionState& actions) {
        const rv::input::ActionState out = repeat.apply(actions, kFrame);
        std::vector<Action> pressed;
        for (const Action arrow : {Action::UiUp, Action::UiDown, Action::UiLeft, Action::UiRight}) {
            if (out.pressed(arrow)) {
                pressed.push_back(arrow);
            }
            CHECK(out.held(arrow) == actions.held(arrow));
        }
        return pressed;
    };
    // Entering Focus with an arrow held (from walking): no step until it is pressed again.
    repeat.reset();
    CHECK(steps(holding({Action::UiRight})).empty());
    CHECK(steps(holding({Action::UiRight})).empty());
    CHECK(steps(rv::input::ActionState{}).empty());
    // A press steps at once; held, again after 0.4 s (24 frames), then every 0.1 s (6 frames).
    std::vector<int> frames;
    for (int frame = 0; frame < 60; ++frame) {
        const auto pressed = steps(frame == 0 ? pressing({Action::UiRight}) : holding({Action::UiRight}));
        if (!pressed.empty()) {
            CHECK(pressed == std::vector<Action>{Action::UiRight});
            frames.push_back(frame);
        }
    }
    CHECK(frames == std::vector<int>{0, 24, 30, 36, 42, 48, 54});
    // Held past 1.5 s, every 0.05 s (3 frames).
    frames.clear();
    for (int frame = 60; frame < 120; ++frame) {
        if (!steps(holding({Action::UiRight})).empty()) {
            frames.push_back(frame);
        }
    }
    REQUIRE(frames.size() >= 4);
    CHECK(frames[frames.size() - 1] - frames[frames.size() - 2] == 3);
    // Released: nothing; tapped within one frame: one step.
    CHECK(steps(rv::input::ActionState{}).empty());
    rv::input::ActionState tap;
    tap.setButton(Action::UiDown, false, true, true);
    CHECK(steps(tap) == std::vector<Action>{Action::UiDown});
    CHECK(steps(rv::input::ActionState{}).empty());
    // Two held: the one pressed last steps.
    CHECK(steps(pressing({Action::UiLeft})) == std::vector<Action>{Action::UiLeft});
    rv::input::ActionState both = holding({Action::UiLeft});
    both.setButton(Action::UiUp, true, true, false);
    CHECK(steps(both) == std::vector<Action>{Action::UiUp});
    // The other actions pass through untouched.
    rv::input::ActionState other = pressing({Action::Interact});
    other.setAxis(Action::LookX, 0.5f);
    const rv::input::ActionState passed = repeat.apply(other, kFrame);
    CHECK(passed.pressed(Action::Interact));
    CHECK(passed.axis(Action::LookX) == 0.5f);
}

TEST_CASE("app/world_focus: the pick ray from the reticle or the cursor", "[app][world_focus]") {
    rv::scene::Camera camera;
    camera.setPose(glm::vec3(1.0f, 1.65f, 2.0f), 0.3f, -0.1f);
    const glm::vec2 window(1280.0f, 720.0f);
    const rv::core::Ray reticle = rv::app::pickRay(camera, rv::app::PickRaySource::Reticle, {10.0f, 10.0f}, window);
    CHECK(glm::length(reticle.direction - camera.forward()) < 1e-5f);
    // The cursor at the window's centre is the reticle; at its top left it is the view's corner.
    const rv::core::Ray centre = rv::app::pickRay(camera, rv::app::PickRaySource::Cursor, window * 0.5f, window);
    CHECK(glm::length(centre.direction - camera.forward()) < 1e-5f);
    const rv::core::Ray corner = rv::app::pickRay(camera, rv::app::PickRaySource::Cursor, {0.0f, 0.0f}, window);
    rv::scene::Camera wide = camera;
    wide.setAspect(window.x / window.y);
    CHECK(glm::length(corner.direction - wide.rayFromNdc({-1.0f, 1.0f}).direction) < 1e-5f);
    CHECK(glm::dot(corner.direction, camera.right()) < 0.0f);
    CHECK(glm::dot(corner.direction, camera.up()) > 0.0f);
    // An empty window or a cursor that is not finite: the reticle.
    const rv::core::Ray empty = rv::app::pickRay(camera, rv::app::PickRaySource::Cursor, {5.0f, 5.0f}, {0.0f, 0.0f});
    CHECK(glm::length(empty.direction - camera.forward()) < 1e-5f);
    const rv::core::Ray nan =
        rv::app::pickRay(camera, rv::app::PickRaySource::Cursor, {std::nanf(""), 1.0f}, window);
    CHECK(glm::length(nan.direction - camera.forward()) < 1e-5f);
}

TEST_CASE("app/world_focus: the label's anchor above the item as the view shows it", "[app][world_focus]") {
    rv::scene::Camera camera;
    camera.setAspect(16.0f / 9.0f);
    camera.setPose(glm::vec3(0.0f, 1.0f, 0.0f), 0.0f, 0.0f);
    const glm::ivec2 size(1920, 1080);
    const glm::mat4 viewProjection = camera.viewProjection();
    // A box straight ahead: the anchor at the view's middle column, at the box's top edge on screen.
    const rv::core::Aabb ahead = rv::core::Aabb::fromMinMax({-0.1f, 0.9f, -2.1f}, {0.1f, 1.1f, -1.9f});
    const auto anchor = rv::app::labelAnchor(ahead, viewProjection, size, 64.0f);
    REQUIRE(anchor.has_value());
    CHECK(std::fabs(anchor->x - 960.0f) < 0.5f);
    CHECK(anchor->y < 540.0f);
    CHECK(anchor->y > 400.0f);
    // Above the view's top: clamped under the margin; behind the eye or off screen: none.
    const rv::core::Aabb high = rv::core::Aabb::fromMinMax({-0.1f, 1.7f, -2.1f}, {0.1f, 2.6f, -1.9f});
    const auto clamped = rv::app::labelAnchor(high, viewProjection, size, 64.0f);
    REQUIRE(clamped.has_value());
    CHECK(clamped->y == 64.0f);
    CHECK_FALSE(rv::app::labelAnchor(rv::core::Aabb::fromMinMax({-0.1f, 0.9f, 1.9f}, {0.1f, 1.1f, 2.1f}),
                                     viewProjection, size, 64.0f));
    CHECK_FALSE(rv::app::labelAnchor(rv::core::Aabb::fromMinMax({5.9f, 0.9f, -2.1f}, {6.1f, 1.1f, -1.9f}),
                                     viewProjection, size, 64.0f));
    CHECK_FALSE(rv::app::labelAnchor(rv::core::Aabb{}, viewProjection, size, 64.0f));
}

namespace {

/// The app's world loop without a window, on a golden plan's room: the reticle's ray sampled by the room's pick index,
/// the interaction updated with the frame's actions (the D-pad's repeat in Focus), its walker pose applied or the
/// walker given its context's actions; a record per frame.
class RoomLoop {
public:
    explicit RoomLoop(const std::string& plan, rv::app::InteractionSettings settings = {})
        : m_layout(rv::test::world::layoutOf(rv::test::world::planCase(plan))), m_interaction(settings) {
        auto room = CollectionRoom::build(std::make_shared<const CollectionLayout>(m_layout));
        REQUIRE(room.has_value());
        m_room = std::move(*room);
        auto index = rv::world::PickIndex::build(m_layout.pickTargets(), m_layout.occluders);
        REQUIRE(index.has_value());
        m_index = std::move(*index);
        m_interaction.setRoom(m_room.get());
    }

    /// The item in `slot` ("<fixture local id>/s:<level>.<index>", e.g. "fx:hw-w/s:5.4").
    [[nodiscard]] const CollectionItem& item(const std::string& slot) const {
        const auto found = std::find_if(m_layout.items.begin(), m_layout.items.end(), [&](const CollectionItem& item) {
            return item.slot.toString().ends_with("/" + slot);
        });
        INFO("the item in " << slot);
        REQUIRE(found != m_layout.items.end());
        return *found;
    }

    void place(const rv::scene::WalkerPose& pose) {
        m_walker.teleport(pose.feet, pose.yaw, pose.pitch);
        m_walker.update(rv::input::ActionState{}, 0.0f, m_camera);
    }

    rv::app::InteractionResult frame(const rv::input::ActionState& actions = {}) {
        rv::app::InteractionFrame input;
        rv::input::ActionState stepped;
        if (m_interaction.context() == rv::app::WorldContext::Focus) {
            stepped = m_repeat.apply(actions, kFrame);
            input.actions = &stepped;
        } else {
            m_repeat.reset();
            input.actions = &actions;
        }
        input.deltaSeconds = kFrame;
        input.walker = rv::scene::WalkerPose{m_walker.feetPosition(), m_walker.yaw(), m_walker.pitch()};
        input.hover = m_index.sample(m_camera.rayFromNdc(glm::vec2(0.0f)), m_interaction.hovered());
        input.back.mouseCaptured = true;
        const rv::app::WorldContext context = m_interaction.context();
        rv::app::InteractionResult result = m_interaction.update(input);
        if (result.walker) {
            place(*result.walker);
        } else {
            m_walker.update(rv::app::walkerActionsFor(context, *input.actions), kFrame, m_camera);
        }
        ++m_frames;
        return result;
    }

    /// Frames without input until `done` holds (at most `limit`); the frames it took.
    template <typename Done>
    int until(Done done, int limit = 120) {
        for (int count = 1; count <= limit; ++count) {
            frame();
            if (done()) {
                return count;
            }
        }
        return -1;
    }

    [[nodiscard]] const rv::app::WorldInteraction& interaction() const noexcept { return m_interaction; }
    [[nodiscard]] float pull(PickId id) const noexcept { return m_interaction.pullOf(id); }

private:
    CollectionLayout m_layout;
    std::unique_ptr<CollectionRoom> m_room;
    rv::world::PickIndex m_index;
    rv::app::WorldInteraction m_interaction;
    FocusStepRepeat m_repeat;
    rv::scene::FirstPersonController m_walker;
    rv::scene::Camera m_camera;
    int m_frames = 0;
};

} // namespace

TEST_CASE("app/world_focus: hover, pull-out, Focus and D-pad steps over the 50-title room, as the app drives them",
          "[app][world_focus]") {
    RoomLoop loop("plan-50-seed-102938");
    const rv::app::WorldInteraction& world = loop.interaction();
    const CollectionItem& band4 = loop.item("fx:hw-w/s:5.4");
    const CollectionItem& band5 = loop.item("fx:hw-w/s:5.5");
    loop.place(rv::app::focusPoseFor(band4, FocusViewer{}));

    // The hover within 6 frames (the hysteresis's 3), the pull-out after the 150 ms dwell, out within 250 ms more.
    const int hovered = loop.until([&] { return world.hovered() == band4.pickId; });
    CHECK(hovered >= 1);
    CHECK(hovered <= 6);
    CHECK(world.highlighted() == band4.pickId);
    const int pullStarts = loop.until([&] { return loop.pull(band4.pickId) > 0.0f; });
    CHECK(pullStarts >= 8);
    CHECK(pullStarts <= 10);
    const int pulledOut = loop.until([&] { return loop.pull(band4.pickId) >= 1.0f; });
    CHECK(pulledOut >= 14);
    CHECK(pulledOut <= 16);

    // A face-out item: interact inspects it; Back (ui_back in Inspect) leaves it in Focus, still pulled out.
    loop.frame(pressing({Action::Interact}));
    CHECK(world.state() == rv::app::WorldState::Inspect);
    loop.until([&] { return world.inspectView().lift >= 1.0f; });
    loop.frame(pressing({Action::UiBack}));
    CHECK(world.state() == rv::app::WorldState::Focus);
    CHECK(world.focused() == band4.pickId);
    loop.until([&] { return !world.walkerEasing() && world.inspectView().lift <= 0.0f; });
    CHECK(loop.pull(band4.pickId) == 1.0f);

    // A still stick held 60 frames changes nothing.
    rv::input::ActionState still;
    still.setAxis(Action::LookX, 0.0f);
    for (int i = 0; i < 60; ++i) {
        loop.frame(still);
    }
    CHECK(world.focused() == band4.pickId);

    // Right along the band; right again across into the wall shelf (the north wall's next fixture with items, its
    // level nearest in height, at its near end); up and down between its ledges; left back into the band.
    loop.frame(pressing({Action::UiRight}));
    CHECK(world.focused() == band5.pickId);
    CHECK(world.walkerEasing());
    CHECK(loop.until([&] { return !world.walkerEasing(); }) <= 22);
    CHECK(loop.pull(band5.pickId) == 1.0f);
    CHECK(loop.pull(band4.pickId) == 0.0f); // pushed back in 200 ms meanwhile
    loop.frame(pressing({Action::UiRight}));
    const PickId shelf = world.focused();
    REQUIRE(shelf != band5.pickId);
    CHECK(CollectionRoom::sectionOf(shelf) != CollectionRoom::sectionOf(band5.pickId));
    loop.until([&] { return !world.walkerEasing(); });
    loop.frame(pressing({Action::UiUp}));
    const PickId upper = world.focused();
    loop.until([&] { return !world.walkerEasing(); });
    loop.frame(pressing({Action::UiDown}));
    loop.until([&] { return !world.walkerEasing(); });
    CHECK(((upper != shelf && world.focused() == shelf) || (upper == shelf)));
    loop.frame(pressing({Action::UiLeft}));
    CHECK(world.focused() == band5.pickId);
    loop.until([&] { return !world.walkerEasing(); });

    // Held, the step repeats: from band slot 5.5 left along the band, at once and again after 0.4 s.
    loop.frame(pressing({Action::UiLeft}));
    CHECK(world.focused() == band4.pickId);
    for (int i = 0; i < 23; ++i) {
        loop.frame(holding({Action::UiLeft}));
    }
    CHECK(world.focused() == band4.pickId);
    loop.frame(holding({Action::UiLeft}));
    CHECK(world.focused() == loop.item("fx:hw-w/s:5.3").pickId);

    // Back, once the walker stands at the item and the reticle's hover settled on it: FreeRoam, the item put back and
    // left in while the hover rests on it, every item back in its slot (drawn by its static records again).
    loop.until([&] { return !world.walkerEasing() && world.hovered() == world.focused(); });
    loop.frame(pressing({Action::Back}));
    CHECK(world.state() == rv::app::WorldState::FreeRoam);
    loop.until([&] { return world.pulledItems().empty(); }, 30);
    CHECK(world.pulledItems().empty());
}

TEST_CASE("app/world_focus: under reduceMotion every transition is instant, the dwell kept", "[app][world_focus]") {
    rv::app::InteractionSettings settings;
    settings.reduceMotion = true;
    RoomLoop loop("plan-50-seed-102938", settings);
    const rv::app::WorldInteraction& world = loop.interaction();
    const CollectionItem& band4 = loop.item("fx:hw-w/s:5.4");
    loop.place(rv::app::focusPoseFor(band4, FocusViewer{}));
    loop.until([&] { return world.hovered() == band4.pickId; });
    // The dwell still applies; then the item is out at once.
    const int out = loop.until([&] { return loop.pull(band4.pickId) > 0.0f; });
    CHECK(out >= 8);
    CHECK(loop.pull(band4.pickId) == 1.0f);
    loop.frame(pressing({Action::Interact}));
    CHECK(world.inspectView().lift == 1.0f);
    loop.frame(pressing({Action::UiBack}));
    CHECK(world.state() == rv::app::WorldState::Focus);
    CHECK(world.inspectView().lift == 0.0f);
    // A step places the walker in the same frame: no ease.
    const rv::app::InteractionResult step = loop.frame(pressing({Action::UiRight}));
    CHECK(step.walker.has_value());
    CHECK_FALSE(world.walkerEasing());
    CHECK(loop.pull(world.focused()) == 1.0f);
    CHECK(loop.pull(band4.pickId) == 0.0f);
    loop.frame(pressing({Action::Back}));
    loop.frame();
    CHECK(world.pulledItems().empty());
}

// Hidden ([.bench], Release: rv_tests "[world_focus][bench]"): what the room's commit adds on the main thread for the
// interaction (WorldHover::setRoom: the pick index, the navigation, every item's row reach) on the 890- and 2,000-title
// rooms, medians of 11 runs; within 20 ms, a fraction of the commit's own time (step 10b: 43 to 59 ms in Release).
TEST_CASE("app/world_focus: the interaction's room built at the commit", "[.bench][app][world_focus][bench]") {
    for (const std::string& name : {std::string("plan-890-seed-102938"), kRooms[2]}) {
        const CollectionLayout layout = rv::test::world::layoutOf(rv::test::world::planCase(name));
        std::vector<double> times;
        for (int run = 0; run < 11; ++run) {
            const auto start = std::chrono::steady_clock::now();
            auto index = rv::world::PickIndex::build(layout.pickTargets(), layout.occluders);
            auto room = CollectionRoom::build(std::make_shared<const CollectionLayout>(layout));
            const auto elapsed = std::chrono::steady_clock::now() - start;
            times.push_back(std::chrono::duration<double, std::milli>(elapsed).count());
            REQUIRE(index.has_value());
            REQUIRE(room.has_value());
        }
        std::sort(times.begin(), times.end());
        WARN(name << ": " << layout.items.size() << " items, " << layout.occluders.size() << " occluders: median "
                  << times[times.size() / 2] << " ms (the layout's copy included)");
#if defined(NDEBUG)
        CHECK(times[times.size() / 2] <= 20.0);
#endif
    }
}
