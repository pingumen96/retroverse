// The room's side of the inspection (I3 step 13b; rv/app/world_inspect.hpp), GPU-free: the held item upright in front
// of the eye with its cover to it, turned by the inspection's yaw and pitch and zoomed, lifted from its pulled-out pose
// without a jump; on the golden plans' layouts (tests/fixtures/world/golden), at every item's Focus pose and at the
// zoom's ends, the held item never reaches into a fixture, a wall or another item; the dimming eased with the lift;
// the hero CRT's selection (the quick catalog's while it is open, else the item in Focus or Inspect); the run report's
// inspect record read back; and the inspection as the app drives it on the 50-title room (snes-room's size): the lift
// in 300 ms, the triggers and the wheel zooming within 0.4 to 1.0 m, the stick turning the item, Back lowering it in
// 200 ms to its pulled-out pose, every frame's pose continuous.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/app/world_inspect.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/core/math.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/pick_index.hpp>

#include "../world/plan_scene_support.hpp"

using rv::app::InspectView;
using rv::app::WorldState;
using rv::input::Action;
using rv::world::CollectionItem;
using rv::world::CollectionLayout;

namespace {

constexpr float kFrame = 1.0f / 60.0f;
constexpr float kEyeHeight = 1.65f;

[[nodiscard]] rv::scene::Camera cameraAt(const glm::vec3& eye, float yawDeg, float pitchDeg) {
    rv::scene::Camera camera;
    camera.setPose(eye, rv::core::degreesToRadians(yawDeg), rv::core::degreesToRadians(pitchDeg));
    return camera;
}

[[nodiscard]] glm::vec3 axis(const glm::mat4& transform, int index) {
    return glm::normalize(glm::vec3(transform[index]));
}

[[nodiscard]] bool near(const glm::vec3& a, const glm::vec3& b, float tolerance) {
    return glm::length(a - b) <= tolerance;
}

/// The distance from `point` to `box` (0 inside).
[[nodiscard]] float distanceToBox(const rv::core::Aabb& box, const glm::vec3& point) {
    const glm::vec3 nearest = glm::clamp(point, box.min, box.max);
    return glm::length(point - nearest);
}

[[nodiscard]] rv::input::ActionState pressing(std::initializer_list<Action> actions) {
    rv::input::ActionState state;
    for (const Action action : actions) {
        state.setButton(action, true, true, false);
    }
    return state;
}

/// A spine-out-like pulled pose: turned about +Y and +Z, moved, scaled by `scale`.
[[nodiscard]] glm::mat4 somePulledPose(float scale = 1.0f) {
    const glm::mat4 turned = glm::mat4(glm::mat3(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
                                                 glm::vec3(0.0f, 0.0f, 1.0f)));
    glm::mat4 transform = turned;
    transform[0] *= scale;
    transform[1] *= scale;
    transform[2] *= scale;
    transform[3] = glm::vec4(0.3f, 1.2f, -2.0f, 1.0f);
    return transform;
}

} // namespace

TEST_CASE("app/world_inspect: held upright in front of the eye, its cover to it, turned and zoomed",
          "[app][world_inspect]") {
    const glm::mat4 pulled = somePulledPose();
    for (const float yawDeg : {0.0f, 37.0f, -120.0f, 180.0f}) {
        for (const float pitchDeg : {0.0f, -35.0f, 20.0f}) {
            INFO("camera yaw " << yawDeg << ", pitch " << pitchDeg);
            const rv::scene::Camera camera = cameraAt({1.0f, 1.65f, 0.5f}, yawDeg, pitchDeg);
            const glm::mat4 held = rv::app::heldItemTransform(pulled, InspectView{1.0f, 0.0f, 0.0f, 0.6f}, camera, 0.6f);
            // In the hand: left of and below the view's centre, beside the card.
            const glm::vec3 toHeld = glm::vec3(held[3]) - camera.position();
            CHECK(near(glm::vec3(held[3]), rv::app::heldCentre(camera, 0.6f), 1e-5f));
            CHECK(std::fabs(glm::dot(toHeld, camera.forward()) - 0.6f) < 1e-5f);
            CHECK(glm::dot(toHeld, camera.right()) < 0.0f);
            CHECK(glm::dot(toHeld, camera.up()) < 0.0f);
            // At the default distance the full offset, zoomed out to the farthest none.
            const glm::vec3 atDefault = rv::app::heldCentre(camera, 0.45f) - camera.position();
            CHECK(std::fabs(glm::dot(atDefault, camera.right()) - 0.45f * rv::app::kHeldViewOffset.x) < 1e-5f);
            CHECK(std::fabs(glm::dot(atDefault, camera.up()) - 0.45f * rv::app::kHeldViewOffset.y) < 1e-5f);
            CHECK(near(axis(held, 2), -camera.forward(), 1e-5f)); // its front faces the eye
            CHECK(near(axis(held, 1), camera.up(), 1e-5f));       // upright in the view
            CHECK(near(axis(held, 0), camera.right(), 1e-5f));

            // Positive yaw turns its front to the viewer's right, positive pitch tilts it up; the centre stays.
            const float turn = rv::core::degreesToRadians(30.0f);
            const glm::mat4 yawed = rv::app::heldItemTransform(pulled, InspectView{1.0f, turn, 0.0f, 0.6f}, camera,
                                                               0.6f);
            CHECK(std::fabs(glm::dot(axis(yawed, 2), camera.right()) - std::sin(turn)) < 1e-5f);
            CHECK(std::fabs(glm::dot(axis(yawed, 2), camera.up())) < 1e-5f);
            CHECK(near(axis(yawed, 1), camera.up(), 1e-5f));
            const glm::mat4 pitched = rv::app::heldItemTransform(pulled, InspectView{1.0f, 0.0f, turn, 0.6f}, camera,
                                                                 0.6f);
            CHECK(std::fabs(glm::dot(axis(pitched, 2), camera.up()) - std::sin(turn)) < 1e-5f);
            CHECK(std::fabs(glm::dot(axis(pitched, 2), camera.right())) < 1e-5f);
            CHECK(near(glm::vec3(pitched[3]), glm::vec3(held[3]), 1e-5f));

            // The zoom is the distance given (heldDistance's).
            const glm::mat4 far = rv::app::heldItemTransform(pulled, InspectView{1.0f, 0.0f, 0.0f, 1.0f}, camera, 1.0f);
            CHECK(near(glm::vec3(far[3]), camera.position() + camera.forward(), 1e-5f));
        }
    }

    // At lift 0 the pulled-out pose exactly; a non-finite lift counts as 0; the prefab's scale is kept.
    const rv::scene::Camera camera = cameraAt({0.0f, 1.65f, 0.0f}, 0.0f, -10.0f);
    CHECK(rv::app::heldItemTransform(pulled, InspectView{0.0f, 1.0f, 0.5f, 0.6f}, camera, 0.6f) == pulled);
    CHECK(rv::app::heldItemTransform(pulled, InspectView{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.6f},
                                     camera, 0.6f) == pulled);
    const glm::mat4 scaled = rv::app::heldItemTransform(somePulledPose(2.0f), InspectView{1.0f, 0.3f, 0.2f, 0.6f},
                                                        camera, 0.6f);
    for (int index = 0; index < 3; ++index) {
        CHECK(std::fabs(glm::length(glm::vec3(scaled[index])) - 2.0f) < 1e-5f);
    }

    // On the way into the hand: the centre along the straight line, the orientation along the shorter arc, no jump.
    const glm::mat4 held = rv::app::heldItemTransform(pulled, InspectView{1.0f, 0.0f, 0.0f, 0.6f}, camera, 0.6f);
    const glm::mat4 half = rv::app::heldItemTransform(pulled, InspectView{0.5f, 0.0f, 0.0f, 0.6f}, camera, 0.6f);
    CHECK(near(glm::vec3(half[3]), 0.5f * (glm::vec3(pulled[3]) + glm::vec3(held[3])), 1e-5f));
    glm::vec3 previous = glm::vec3(pulled[3]);
    glm::vec3 previousFront = axis(pulled, 2);
    for (int step = 1; step <= 100; ++step) {
        const glm::mat4 at = rv::app::heldItemTransform(
            pulled, InspectView{static_cast<float>(step) / 100.0f, 0.0f, 0.0f, 0.6f}, camera, 0.6f);
        CHECK(glm::length(glm::vec3(at[3]) - previous) < 0.03f);
        CHECK(glm::dot(axis(at, 2), previousFront) > std::cos(rv::core::degreesToRadians(3.0f)));
        previous = glm::vec3(at[3]);
        previousFront = axis(at, 2);
    }
}

TEST_CASE("app/world_inspect: the held distance kept out of the item's fixture", "[app][world_inspect]") {
    using rv::app::heldDistance;
    using rv::app::heldDistanceLimit;
    using rv::app::kHeldClearance;
    const rv::core::Aabb shelf = rv::core::Aabb::fromMinMax({-0.5f, 1.0f, -3.2f}, {0.5f, 1.2f, -3.0f});
    const glm::vec3 eye(0.0f, 1.1f, -2.1f);
    // Looking at it: the ray enters the grown box 0.9 - clearance from the eye, less the item's radius.
    CHECK(std::fabs(heldDistanceLimit(shelf, 0.08f, eye, {0.0f, 0.0f, -1.0f}) - (0.9f - kHeldClearance - 0.08f)) <
          1e-5f);
    // Looking away, from inside the box, or anything not finite: no limit.
    CHECK(std::isinf(heldDistanceLimit(shelf, 0.08f, eye, {0.0f, 0.0f, 1.0f})));
    CHECK(std::isinf(heldDistanceLimit(shelf, 0.08f, {0.0f, 1.1f, -3.1f}, {0.0f, 0.0f, -1.0f})));
    CHECK(std::isinf(heldDistanceLimit(rv::core::Aabb{}, 0.08f, eye, {0.0f, 0.0f, -1.0f})));
    CHECK(std::isinf(heldDistanceLimit(shelf, 0.08f, eye, {0.0f, 0.0f, 0.0f})));
    // The zoom's distance, at most the limit, never nearer than kHeldMinDistance.
    CHECK(heldDistance(0.6f, std::numeric_limits<float>::infinity()) == 0.6f);
    CHECK(heldDistance(1.0f, 0.75f) == 0.75f);
    CHECK(heldDistance(0.4f, 0.75f) == 0.4f);
    CHECK(heldDistance(0.6f, 0.05f) == rv::app::kHeldMinDistance);
    CHECK(heldDistance(std::numeric_limits<float>::quiet_NaN(), 2.0f) == rv::app::InspectSettings{}.distance);
}

TEST_CASE("app/world_inspect: at every item's Focus pose the held item never reaches into the room",
          "[app][world_inspect]") {
    for (const std::string& name :
         {std::string("plan-50-seed-102938"), std::string("plan-890-seed-102938"),
          std::string("plan-2000-seed-18446744073709551615")}) {
        INFO(name);
        const CollectionLayout layout = rv::test::world::layoutOf(rv::test::world::planCase(name));
        auto room = rv::app::CollectionRoom::build(std::make_shared<const CollectionLayout>(layout));
        REQUIRE(room.has_value());
        std::size_t unclampedDefault = 0;
        for (const CollectionItem& item : layout.items) {
            INFO(item.slot.toString());
            const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, rv::app::FocusViewer{});
            rv::scene::Camera camera;
            camera.setPose(pose.feet + glm::vec3(0.0f, kEyeHeight, 0.0f), pose.yaw, pose.pitch);
            const float radius = rv::app::heldRadius(item);
            const auto cluster = std::find_if(layout.clusters.begin(), layout.clusters.end(),
                                              [&](const auto& candidate) { return candidate.id == item.cluster; });
            REQUIRE(cluster != layout.clusters.end());
            const float limit =
                rv::app::heldDistanceLimit(cluster->bounds, radius, camera.position(), camera.forward());
            const glm::mat4 pulled = rv::app::pulledItemTransform(item, 1.0f, room->get()->reachOf(item.pickId));
            for (const float zoom : {0.3f, 0.45f, 1.0f}) {
                INFO("zoom " << zoom);
                const float drawn = rv::app::heldDistance(zoom, limit);
                CHECK(drawn >= 0.3f); // the fixture always leaves the zoom's nearest end
                CHECK(drawn <= zoom);
                unclampedDefault += zoom == 0.45f && drawn == zoom ? 1 : 0;
                const glm::mat4 held = rv::app::heldItemTransform(pulled, InspectView{1.0f, 0.7f, 0.4f, zoom},
                                                                  camera, drawn);
                const glm::vec3 centre(held[3]);
                // The sphere that holds it whatever its turn stays clear of every occluder and every item's box.
                for (const rv::core::Aabb& occluder : layout.occluders) {
                    CHECK(distanceToBox(occluder, centre) > radius);
                }
                for (const CollectionItem& other : layout.items) {
                    CHECK(distanceToBox(other.worldBounds(), centre) > radius);
                }
                CHECK(drawn - radius > camera.lens().nearPlane);
            }
        }
        // The default hold (0.45 m) is never cut short at a Focus pose (0.9 m from the face).
        CHECK(unclampedDefault == layout.items.size());
    }
}

TEST_CASE("app/world_inspect: the scene behind dimmed by one stop and vignetted, eased with the lift",
          "[app][world_inspect]") {
    CHECK(rv::app::inspectDimming(0.0f, 0.05f) == rv::app::InspectDimming{0.0f, 0.05f});
    CHECK(rv::app::inspectDimming(1.0f, 0.05f) == rv::app::InspectDimming{0.5f, 0.05f + rv::app::kInspectVignette});
    const rv::app::InspectDimming half = rv::app::inspectDimming(0.5f, 0.0f);
    CHECK(half.dimOutside == 0.25f);
    CHECK(half.vignette == 0.5f * rv::app::kInspectVignette);
    CHECK(rv::app::inspectDimming(2.0f, 0.95f) == rv::app::InspectDimming{0.5f, 1.0f});
    CHECK(rv::app::inspectDimming(std::numeric_limits<float>::quiet_NaN(), 0.05f) ==
          rv::app::InspectDimming{0.0f, 0.05f});
}

TEST_CASE("app/world_inspect: the hero CRT follows the catalog's selection, else the item in Focus or Inspect",
          "[app][world_inspect]") {
    using rv::app::screenSelection;
    const std::optional<std::string> none;
    CHECK(screenSelection(std::string("catalog"), WorldState::Inspect, "world") == std::string("catalog"));
    CHECK(screenSelection(none, WorldState::Focus, "world") == std::string("world"));
    CHECK(screenSelection(none, WorldState::Inspect, "world") == std::string("world"));
    CHECK(screenSelection(none, WorldState::Launching, "world") == std::string("world"));
    CHECK(screenSelection(none, WorldState::Returning, "world") == std::string("world"));
    CHECK_FALSE(screenSelection(none, WorldState::FreeRoam, "world"));
    CHECK_FALSE(screenSelection(none, WorldState::Focus, ""));
}

TEST_CASE("app/world_inspect: the run report's inspect record reads back", "[app][world_inspect]") {
    rv::app::RunReport report;
    report.world.bound = true;
    report.world.interaction.bound = true;
    rv::app::InspectRecord& inspect = report.world.interaction.inspect;
    inspect.inspections = 2;
    inspect.liftFrames = 30;
    inspect.heldFrames = 100;
    inspect.dimmedFrames = 130;
    inspect.mediaRequests = 1;
    inspect.minDistance = 0.4;
    inspect.maxDistance = 1.0;
    inspect.minDrawnDistance = 0.4;
    inspect.maxDrawnDistance = 0.774;
    inspect.pickId = "0300000C";
    inspect.lift = 1.0;
    inspect.yawDeg = 45.5;
    inspect.pitchDeg = -12.25;
    inspect.distance = 0.6;
    inspect.drawnDistance = 0.6;
    inspect.heldCenter = glm::vec3(0.0f, 0.0f, -0.6f);
    inspect.heldFront = glm::vec3(0.0f, 0.0f, 1.0f);
    inspect.heldUp = glm::vec3(0.0f, 1.0f, 0.0f);
    inspect.heroFront = true;
    const auto parsed = rv::app::parseRunReport(rv::app::formatRunReport(report), "report.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->world.interaction.inspect == inspect);
}

namespace {

/// The app's world loop without a window on the 50-title room (as test_world_focus.cpp's): the reticle's ray sampled
/// by the room's pick index, the interaction updated, the walker placed or given its context's actions; the held pose
/// computed each frame as WorldInspection draws it.
class InspectLoop {
public:
    InspectLoop()
        : m_layout(rv::test::world::layoutOf(rv::test::world::planCase("plan-50-seed-102938"))) {
        auto room = rv::app::CollectionRoom::build(std::make_shared<const CollectionLayout>(m_layout));
        REQUIRE(room.has_value());
        m_room = std::move(*room);
        auto index = rv::world::PickIndex::build(m_layout.pickTargets(), m_layout.occluders);
        REQUIRE(index.has_value());
        m_index = std::move(*index);
        m_interaction.setRoom(m_room.get());
    }

    [[nodiscard]] const CollectionItem& item(const std::string& slot) const {
        const auto found = std::find_if(m_layout.items.begin(), m_layout.items.end(), [&](const CollectionItem& item) {
            return item.slot.toString().ends_with("/" + slot);
        });
        REQUIRE(found != m_layout.items.end());
        return *found;
    }

    void place(const rv::scene::WalkerPose& pose) {
        m_pose = pose;
        m_camera.setPose(pose.feet + glm::vec3(0.0f, kEyeHeight, 0.0f), pose.yaw, pose.pitch);
    }

    void frame(const rv::input::ActionState& actions = {}, float wheel = 0.0f) {
        rv::app::InteractionFrame input;
        input.actions = &actions;
        input.deltaSeconds = kFrame;
        input.walker = m_pose;
        input.wheelNotches = wheel;
        input.hover = m_index.sample(m_camera.rayFromNdc(glm::vec2(0.0f)), m_interaction.hovered());
        input.back.mouseCaptured = true;
        const rv::app::InteractionResult result = m_interaction.update(input);
        if (result.walker) {
            place(*result.walker);
        }
        // The held pose of the focused item, as drawn.
        if (const CollectionItem* item = m_room->find(m_interaction.focused())) {
            const auto cluster = std::find_if(m_layout.clusters.begin(), m_layout.clusters.end(),
                                              [&](const auto& candidate) { return candidate.id == item->cluster; });
            const float limit = rv::app::heldDistanceLimit(cluster->bounds, rv::app::heldRadius(*item),
                                                           m_camera.position(), m_camera.forward());
            const InspectView view = m_interaction.inspectView();
            const glm::mat4 pulled = rv::app::pulledItemTransform(*item, m_interaction.pullOf(item->pickId),
                                                                  m_room->reachOf(item->pickId));
            m_held = rv::app::heldItemTransform(pulled, view, m_camera, rv::app::heldDistance(view.distance, limit));
            if (m_previous) {
                m_maxStep = std::max(m_maxStep, glm::length(glm::vec3(m_held[3]) - glm::vec3((*m_previous)[3])));
            }
            m_previous = m_held;
        }
    }

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
    [[nodiscard]] const rv::scene::Camera& camera() const noexcept { return m_camera; }
    [[nodiscard]] const glm::mat4& held() const noexcept { return m_held; }
    [[nodiscard]] float maxStep() const noexcept { return m_maxStep; }

private:
    CollectionLayout m_layout;
    std::unique_ptr<rv::app::CollectionRoom> m_room;
    rv::world::PickIndex m_index;
    rv::app::WorldInteraction m_interaction;
    rv::scene::WalkerPose m_pose;
    rv::scene::Camera m_camera;
    glm::mat4 m_held{1.0f};
    std::optional<glm::mat4> m_previous;
    float m_maxStep = 0.0f;
};

} // namespace

TEST_CASE("app/world_inspect: lift, turn, zoom and Back over the 50-title room, as the app drives them",
          "[app][world_inspect]") {
    InspectLoop loop;
    const rv::app::WorldInteraction& world = loop.interaction();
    const CollectionItem& band4 = loop.item("fx:hw-w/s:5.4");
    loop.place(rv::app::focusPoseFor(band4, rv::app::FocusViewer{}));
    loop.until([&] { return world.pullOf(band4.pickId) >= 1.0f; });

    // Interact on the face-out item: Inspect, the lift in 300 ms (18 frames), held upright facing the eye at 0.45 m, in
    // the hand left of the view's centre.
    loop.frame(pressing({Action::Interact}));
    REQUIRE(world.state() == WorldState::Inspect);
    const int lifted = loop.until([&] { return world.inspectView().lift >= 1.0f; });
    CHECK(lifted >= 17);
    CHECK(lifted <= 19);
    CHECK(near(glm::vec3(loop.held()[3]), rv::app::heldCentre(loop.camera(), 0.45f), 1e-4f));
    CHECK(near(axis(loop.held(), 2), -loop.camera().forward(), 1e-4f));

    // The right stick turns it (a second at full deflection: 180 degrees, wrapped), the walker does not move.
    const glm::vec3 eye = loop.camera().position();
    rv::input::ActionState turning;
    turning.setAxis(Action::LookX, 1.0f);
    for (int i = 0; i < 30; ++i) {
        loop.frame(turning);
    }
    CHECK(std::fabs(world.inspectView().yaw - rv::core::degreesToRadians(90.0f)) < 1e-3f);
    CHECK(std::fabs(glm::dot(axis(loop.held(), 2), loop.camera().right()) - 1.0f) < 1e-3f);
    CHECK(loop.camera().position() == eye);

    // The right trigger brings it to 0.3 m, the left one takes it to 1.0 m, the wheel 5 cm a notch.
    rv::input::ActionState closer;
    closer.setAxis(Action::UiPageDown, 1.0f);
    for (int i = 0; i < 60; ++i) {
        loop.frame(closer);
    }
    CHECK(world.inspectView().distance == 0.3f);
    rv::input::ActionState farther;
    farther.setAxis(Action::UiPageUp, 1.0f);
    for (int i = 0; i < 90; ++i) {
        loop.frame(farther);
    }
    CHECK(world.inspectView().distance == 1.0f);
    // Drawn short of its fixture (0.9 m from the face): outside the band's box grown by the clearance.
    const float drawn = glm::dot(glm::vec3(loop.held()[3]) - loop.camera().position(), loop.camera().forward());
    CHECK(drawn < 0.95f);
    CHECK(drawn > 0.6f);
    loop.frame({}, 4.0f);
    CHECK(std::fabs(world.inspectView().distance - 0.8f) < 1e-5f);

    // ui_back (Indietro): Focus, lowered in 200 ms (12 frames) to its pulled-out pose; no frame jumps.
    loop.frame(pressing({Action::UiBack}));
    CHECK(world.state() == WorldState::Focus);
    const int lowered = loop.until([&] { return world.inspectView().lift <= 0.0f; });
    CHECK(lowered >= 11);
    CHECK(lowered <= 13);
    CHECK(loop.held() == rv::app::pulledItemTransform(band4, 1.0f, 0.0f));
    CHECK(loop.maxStep() < 0.05f);

    // Back: FreeRoam, the item put back.
    loop.frame(pressing({Action::Back}));
    CHECK(world.state() == WorldState::FreeRoam);
}

// ---------------------------------------------------------------------------------------------------------------
// The insertion into the console (I5 step 9, ADR-0016 §11)
// ---------------------------------------------------------------------------------------------------------------

TEST_CASE("app/world_inspect: a console room's console has its media socket; the arcade and a shared room none",
          "[app][world_inspect][insertion]") {
    auto text = rv::core::readTextFile(rv::test::world::goldenPlanDir() / "building-esde-seed-102938.json");
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, "building-esde-seed-102938");
    REQUIRE(plan.has_value());
    std::size_t consoles = 0;
    for (const rv::world::Room& room : plan->rooms) {
        INFO(room.id.toString());
        auto data = rv::world::loadSceneData(rv::test::world::sourceAssetRoot(), *plan, room.id);
        REQUIRE(data.has_value());
        auto scene = rv::world::toSceneDescription(*plan, room.id, *data);
        REQUIRE(scene.has_value());
        const std::optional<rv::app::ConsoleSocket> socket = rv::app::consoleSocketOf(*scene, "");
        const bool consoleRoom = room.id.toString().starts_with("room:");
        CHECK(socket.has_value() == consoleRoom);
        // A prefix of another room finds nothing.
        CHECK_FALSE(rv::app::consoleSocketOf(*scene, "room:none/").has_value());
        if (!socket) {
            continue;
        }
        ++consoles;
        // On the console's top, its opening the slot's (a little under 15 x 4.5 cm), +Y up.
        const rv::scene::SceneObject* console = nullptr;
        for (const rv::scene::SceneObject& object : scene->objects) {
            if (object.id.ends_with(rv::app::kConsoleIdSuffix)) {
                console = &object;
            }
        }
        REQUIRE(console != nullptr);
        CHECK(console->kind == "console_cartridge");
        const glm::vec3 base(console->instanceTransforms().front()[3]);
        CHECK(socket->position().y > base.y + 0.03f);
        CHECK(socket->position().y < base.y + 0.3f);
        CHECK(glm::length(glm::vec2(socket->position().x - base.x, socket->position().z - base.z)) < 0.2f);
        CHECK(axis(socket->transform, 1).y > 0.999f);
        CHECK(socket->opening.x > 0.1f);
        CHECK(socket->opening.x <= 0.15f + 1e-6f);
        CHECK(socket->opening.y > 0.02f);
        CHECK(socket->opening.y <= 0.045f + 1e-6f);
    }
    CHECK(consoles == 3);
    // A hand-made scene without a console.
    CHECK_FALSE(rv::app::consoleSocketOf(rv::scene::SceneDescription{}, "").has_value());
}

TEST_CASE("app/world_inspect: the item shrinks into the socket's opening", "[app][world_inspect][insertion]") {
    using rv::app::insertedScale;
    using rv::app::kInsertedFill;
    using rv::app::kInsertedHalfDepth;
    const glm::vec2 opening(0.15f, 0.045f);
    // A cartridge that fits keeps its size; a wider case shrinks to 90% of the opening's width, a thicker one to its
    // depth's, a taller one to the console's depth.
    CHECK(insertedScale(glm::vec3(0.10f, 0.09f, 0.02f), opening) == 1.0f);
    CHECK(insertedScale(glm::vec3(0.30f, 0.09f, 0.02f), opening) == kInsertedFill * 0.15f / 0.30f);
    CHECK(insertedScale(glm::vec3(0.10f, 0.09f, 0.08f), opening) == kInsertedFill * 0.045f / 0.08f);
    CHECK(insertedScale(glm::vec3(0.10f, 0.22f, 0.02f), opening) == 2.0f * kInsertedHalfDepth / 0.22f);
    // Nothing known sets no bound.
    CHECK(insertedScale(glm::vec3(0.30f, 0.0f, 0.02f), glm::vec2(0.0f)) == 1.0f);
    CHECK(insertedScale(glm::vec3(std::numeric_limits<float>::quiet_NaN()), opening) == 1.0f);
    // The golden plan's cartridges: their sizes along their prefab's axes, their front wider than they are thick.
    const CollectionLayout layout = rv::test::world::layoutOf(rv::test::world::planCase("plan-50-seed-102938"));
    REQUIRE_FALSE(layout.items.empty());
    for (const CollectionItem& item : layout.items) {
        const glm::vec3 extents = rv::app::itemExtents(item);
        INFO(item.slot.toString());
        CHECK(extents.x > 0.05f);
        CHECK(extents.y > 0.05f);
        CHECK(extents.z > 0.005f);
        CHECK(extents.x > extents.z);
        const float scale = insertedScale(extents, opening);
        CHECK(scale > 0.3f);
        CHECK(scale <= 1.0f);
        CHECK(extents.x * scale <= kInsertedFill * opening.x + 1e-6f);
    }
    CHECK(rv::app::itemExtents(CollectionItem{}) == glm::vec3(0.0f));
}

TEST_CASE("app/world_inspect: from the hand into the socket: its centre at the socket at the end, upright, facing the "
          "console's front",
          "[app][world_inspect][insertion]") {
    // The socket of a console turned a little, on a cabinet 0.55 m high.
    rv::app::ConsoleSocket socket;
    socket.transform = glm::mat4(glm::mat3(glm::vec3(std::cos(0.14f), 0.0f, -std::sin(0.14f)), glm::vec3(0.0f, 1.0f, 0.0f),
                                           glm::vec3(std::sin(0.14f), 0.0f, std::cos(0.14f))));
    socket.transform[3] = glm::vec4(-0.2f, 0.62f, -4.6f, 1.0f);
    socket.opening = glm::vec2(0.15f, 0.045f);
    const glm::mat4 held = somePulledPose(1.25f);
    const float scale = 0.8f;
    using rv::app::insertedItemTransform;
    CHECK(insertedItemTransform(held, socket, scale, 0.0f) == held);
    CHECK(insertedItemTransform(held, socket, scale, -1.0f) == held);
    CHECK(insertedItemTransform(held, socket, scale, std::numeric_limits<float>::quiet_NaN()) == held);
    for (const float progress : {1.0f, 2.0f}) {
        const glm::mat4 inserted = insertedItemTransform(held, socket, scale, progress);
        CHECK(near(glm::vec3(inserted[3]), socket.position(), 1e-5f));
        for (int index = 0; index < 3; ++index) {
            CHECK(near(axis(inserted, index), axis(socket.transform, index), 1e-5f));
            CHECK(std::abs(glm::length(glm::vec3(inserted[index])) - 1.25f * scale) < 1e-5f);
        }
    }
    // On its way: along the straight line, the scale between, never a jump from frame to frame.
    glm::vec3 previous(held[3]);
    for (int frame = 1; frame <= 27; ++frame) {
        const float t = static_cast<float>(frame) / 27.0f;
        const glm::mat4 pose = insertedItemTransform(held, socket, scale, t);
        const glm::vec3 centre(pose[3]);
        const glm::vec3 expected = glm::mix(glm::vec3(held[3]), socket.position(), t);
        CHECK(near(centre, expected, 1e-4f));
        CHECK(glm::length(centre - previous) < 0.25f);
        const float length = glm::length(glm::vec3(pose[0]));
        CHECK(length <= 1.25f + 1e-5f);
        CHECK(length >= 1.25f * scale - 1e-5f);
        previous = centre;
    }
    // The dimming fades out as it goes.
    CHECK(rv::app::insertionDimming(1.0f, 0.0f, 0.05f) == rv::app::inspectDimming(1.0f, 0.05f));
    CHECK(rv::app::insertionDimming(1.0f, 1.0f, 0.05f) == rv::app::inspectDimming(0.0f, 0.05f));
    CHECK(rv::app::insertionDimming(1.0f, 0.5f, 0.0f) == rv::app::inspectDimming(0.5f, 0.0f));
}

TEST_CASE("app/world_inspect: the room's console pose turns the eye to the socket, the feet kept",
          "[app][world_inspect][insertion]") {
    const CollectionLayout layout = rv::test::world::layoutOf(rv::test::world::planCase("plan-50-seed-102938"));
    auto room = rv::app::CollectionRoom::build(std::make_shared<const CollectionLayout>(layout));
    REQUIRE(room.has_value());
    const rv::scene::WalkerPose from{glm::vec3(1.0f, 0.0f, -2.0f), 0.3f, -0.1f};
    CHECK_FALSE((*room)->consolePose(from).has_value());
    rv::app::ConsoleSocket socket;
    socket.transform[3] = glm::vec4(-0.2f, 0.62f, -4.6f, 1.0f);
    (*room)->setConsoleSocket(socket);
    const std::optional<rv::scene::WalkerPose> pose = (*room)->consolePose(from);
    REQUIRE(pose.has_value());
    CHECK(pose->feet == from.feet);
    const rv::scene::Camera camera = cameraAt(from.feet + glm::vec3(0.0f, kEyeHeight, 0.0f),
                                              rv::core::radiansToDegrees(pose->yaw),
                                              rv::core::radiansToDegrees(pose->pitch));
    const glm::vec3 toSocket = glm::normalize(socket.position() - camera.position());
    CHECK(glm::dot(camera.forward(), toSocket) > 0.9999f);
}

TEST_CASE("app/world_inspect: the run report's insertion record reads back", "[app][world_inspect][insertion]") {
    rv::app::RunReport report;
    report.world.bound = true;
    report.world.interaction.bound = true;
    rv::app::InsertionRecord& insertion = report.world.interaction.insertion;
    insertion.started = 2;
    insertion.arrived = 1;
    insertion.skipped = 1;
    insertion.refused = 0;
    insertion.movingFrames = 31;
    insertion.socketFrames = 3;
    insertion.pickId = "0300000C";
    insertion.startFrame = 120;
    insertion.endFrame = 125;
    insertion.seconds = 0.1;
    insertion.end = "skipped";
    insertion.socket = glm::vec3(-0.25f, 0.62f, -4.5f);
    insertion.center = glm::vec3(-0.25f, 0.62f, -4.5f);
    insertion.scale = 0.75;
    insertion.distance = 0.0;
    insertion.frames = {{120, 0.02, 1.5}, {125, 1.0, 0.0}};
    rv::app::VideoLaunchRecord launch;
    launch.decoderRan = true;
    launch.stopMs = 12.5;
    launch.stopFrame = 120;
    launch.waitMs = 0.25;
    launch.handOverFrame = 126;
    launch.cancelled = true;
    report.video.bound = true;
    report.video.launches.push_back(launch);
    const auto parsed = rv::app::parseRunReport(rv::app::formatRunReport(report), "report.json");
    REQUIRE(parsed.has_value());
    CHECK(parsed->world.interaction.insertion == insertion);
    REQUIRE(parsed->video.launches.size() == 1);
    CHECK(parsed->video.launches.front() == launch);
}
