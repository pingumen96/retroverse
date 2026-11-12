// The motion of a scene object (rv/scene/object_motion.hpp, I12 step 8, ADR-0021 §6): a spin's angle from the run's
// seconds, a clock's hands from the time of day (clockwise, the second hand stepping), the delta that turns an item
// about its pivot, and "motion" read from a scene file and refused where it cannot move.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <rv/scene/object_motion.hpp>
#include <rv/scene/scene_description.hpp>

using Catch::Approx;
using rv::scene::ObjectMotion;

namespace {

constexpr float kTwoPi = 6.28318530718f;

ObjectMotion motionOf(ObjectMotion::Kind kind, float turns = 0.0f) {
    ObjectMotion motion;
    motion.kind = kind;
    motion.turnsPerSecond = turns;
    return motion;
}

} // namespace

TEST_CASE("scene/object_motion: a spin turns with the run's seconds, a clock's hands with the time of day",
          "[scene][life]") {
    const ObjectMotion fan = motionOf(ObjectMotion::Kind::Spin, 0.5f);
    CHECK(rv::scene::motionAngle(fan, 0.0, 0.0) == 0.0f);
    CHECK(rv::scene::motionAngle(fan, 0.5, 0.0) == Approx(0.25f * kTwoPi));
    CHECK(rv::scene::motionAngle(fan, 2.5, 0.0) == Approx(0.25f * kTwoPi)); // wrapped to one turn
    CHECK(rv::scene::motionAngle(motionOf(ObjectMotion::Kind::Spin, -0.5f), 0.5, 0.0) == Approx(0.75f * kTwoPi));
    CHECK(rv::scene::motionAngle(fan, std::nan(""), 0.0) == 0.0f);

    // 10:10:30: the hour hand a third past ten (305 degrees), the minute hand at ten past (60.5 degrees with its half
    // minute), the second hand at the half (180): clockwise seen from the face, so negative about its axis.
    const double tenPast = 10.0 * 3600.0 + 10.0 * 60.0 + 30.0;
    const auto degrees = [](float radians) { return -radians * 360.0f / kTwoPi; };
    CHECK(degrees(rv::scene::motionAngle(motionOf(ObjectMotion::Kind::ClockHour), 0.0, tenPast)) ==
          Approx(305.25f).margin(1e-3));
    CHECK(degrees(rv::scene::motionAngle(motionOf(ObjectMotion::Kind::ClockMinute), 0.0, tenPast)) ==
          Approx(63.0f).margin(1e-3));
    CHECK(degrees(rv::scene::motionAngle(motionOf(ObjectMotion::Kind::ClockSecond), 0.0, tenPast)) ==
          Approx(180.0f).margin(1e-3));
    // The second hand steps on whole seconds; the others sweep.
    const ObjectMotion second = motionOf(ObjectMotion::Kind::ClockSecond);
    CHECK(rv::scene::motionAngle(second, 0.0, tenPast + 0.9) == rv::scene::motionAngle(second, 0.0, tenPast));
    CHECK(rv::scene::motionAngle(second, 0.0, tenPast + 1.0) != rv::scene::motionAngle(second, 0.0, tenPast));
    // Noon and midnight: every hand at twelve.
    for (const ObjectMotion::Kind kind :
         {ObjectMotion::Kind::ClockHour, ObjectMotion::Kind::ClockMinute, ObjectMotion::Kind::ClockSecond}) {
        CHECK(rv::scene::motionAngle(motionOf(kind), 0.0, 12.0 * 3600.0) == Approx(0.0f).margin(1e-6));
        CHECK(rv::scene::motionAngle(motionOf(kind), 0.0, 0.0) == Approx(0.0f).margin(1e-6));
    }
}

TEST_CASE("scene/object_motion: an item turns about its object's pivot and axis, its rest pose at angle 0",
          "[scene][life]") {
    // A blade 0.3 m out along +X of a fan's axis at (2, 2.8, -1), turned 90 degrees about +Y: its tip at x = 2.5 ends at
    // z = -1.5 (the right-hand rule about +Y takes +X to -Z).
    ObjectMotion motion = motionOf(ObjectMotion::Kind::Spin, 1.0f);
    motion.pivot = glm::vec3(-0.3f, 0.0f, 0.0f);
    motion.axis = glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 blade = glm::translate(glm::mat4(1.0f), glm::vec3(2.3f, 2.8f, -1.0f));
    const glm::vec4 tip(2.5f, 2.8f, -1.0f, 1.0f);
    const glm::vec4 turned = rv::scene::motionDelta(blade, motion, 0.25f * kTwoPi) * tip;
    CHECK(turned.x == Approx(2.0f).margin(1e-5));
    CHECK(turned.y == Approx(2.8f).margin(1e-5));
    CHECK(turned.z == Approx(-1.5f).margin(1e-5));
    const glm::vec4 rest = rv::scene::motionDelta(blade, motion, 0.0f) * tip;
    CHECK(rest.x == Approx(tip.x).margin(1e-6));
    CHECK(rest.z == Approx(tip.z).margin(1e-6));
    // A hand on a wall facing +X (the object turned 90 degrees about +Y): its axis, local +Z, points along +X in the
    // world; a quarter turn clockwise seen from the room takes its tip from twelve to three.
    ObjectMotion hand = motionOf(ObjectMotion::Kind::ClockMinute);
    hand.pivot = glm::vec3(0.0f, -0.05f, 0.0f);
    hand.axis = glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::mat4 onWall = glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 2.05f, 3.0f)),
                                         0.25f * kTwoPi, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::vec4 handTip(0.0f, 2.1f, 3.0f, 1.0f); // 0.1 m over the dial's centre at (0, 2.0, 3.0)
    const glm::vec4 quarterPast = rv::scene::motionDelta(onWall, hand, -0.25f * kTwoPi) * handTip;
    CHECK(quarterPast.y == Approx(2.0f).margin(1e-5));
    CHECK(quarterPast.x == Approx(0.0f).margin(1e-5));
    // Seen from +X (in front of the face), clockwise from up goes to the viewer's right: towards -Z.
    CHECK(quarterPast.z == Approx(2.9f).margin(1e-5));
}

TEST_CASE("scene/object_motion: a scene file's motion is read, and refused where it cannot move", "[scene][life]") {
    const auto parse = [](nlohmann::json object) {
        nlohmann::json base{{"id", "blade"}, {"primitive", "box"}, {"castsShadows", false}, {"collides", false}};
        base.update(object);
        nlohmann::json document{
            {"schemaVersion", 1}, {"name", "life"}, {"objects", nlohmann::json::array({base})}};
        return rv::scene::parseSceneDescription(document, "life.json");
    };
    const auto spin =
        parse({{"motion", {{"kind", "spin"}, {"pivot", {-0.3, 0, 0}}, {"axis", {0, 2, 0}}, {"turnsPerSecond", -0.9}}}});
    REQUIRE(spin.has_value());
    REQUIRE(spin->objects[0].motion.has_value());
    CHECK(spin->objects[0].motion->kind == ObjectMotion::Kind::Spin);
    CHECK(spin->objects[0].motion->axis == glm::vec3(0.0f, 1.0f, 0.0f)); // made unit
    CHECK(spin->objects[0].motion->pivot.x == Approx(-0.3f));
    CHECK(spin->objects[0].motion->turnsPerSecond == Approx(-0.9f));
    const auto hand = parse({{"motion", {{"kind", "clock-second"}, {"axis", {0, 0, 1}}}}});
    REQUIRE(hand.has_value());
    CHECK(hand->objects[0].motion->kind == ObjectMotion::Kind::ClockSecond);
    CHECK_FALSE(parse(nlohmann::json::object())->objects[0].motion.has_value());

    const auto refused = [&](nlohmann::json object, const std::string& where) {
        const auto result = parse(std::move(object));
        REQUIRE_FALSE(result.has_value());
        CAPTURE(result.error().toString());
        CHECK(result.error().toString().find(where) != std::string::npos);
    };
    refused({{"motion", {{"kind", "wobble"}, {"axis", {0, 1, 0}}}}}, "motion/kind");
    refused({{"motion", {{"kind", "spin"}, {"axis", {0, 0, 0}}}}}, "motion/axis");
    refused({{"motion", {{"kind", "spin"}}}}, "axis");
    refused({{"motion", {{"kind", "spin"}, {"axis", {0, 1, 0}}, {"turnsPerSecond", 9}}}}, "turnsPerSecond");
    refused({{"motion", {{"kind", "clock-hour"}, {"axis", {0, 0, 1}}, {"turnsPerSecond", 1}}}}, "turnsPerSecond");
    refused({{"collides", true}, {"motion", {{"kind", "spin"}, {"axis", {0, 1, 0}}}}}, "motion");
    refused({{"castsShadows", true}, {"motion", {{"kind", "spin"}, {"axis", {0, 1, 0}}}}}, "motion");
    refused({{"grid", {{"counts", {2, 1, 1}}, {"spacing", {1, 0, 0}}}}, {"motion", {{"kind", "spin"}, {"axis", {0, 1, 0}}}}},
            "motion");
}
