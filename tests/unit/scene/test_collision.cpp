#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/scene/collision.hpp>

using Catch::Approx;
using rv::core::Aabb;
using rv::core::degreesToRadians;
using rv::core::nearlyEqual;
using rv::scene::Capsule;
using rv::scene::CollisionWorld;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
const Capsule kPlayer{}; // radius 0.30 m, height 1.75 m
const glm::vec3 kUp(0.0f, 1.0f, 0.0f);

/// Wall occupying x in [1.0, 1.2], 3 m tall, 10 m long along Z.
CollisionWorld eastWall() {
    CollisionWorld world;
    world.addBox(Aabb::fromMinMax({1.0f, 0.0f, -5.0f}, {1.2f, 3.0f, 5.0f}), "wall_east");
    return world;
}

bool nearlyEqual2(const glm::vec2& a, const glm::vec2& b, float epsilon = 1e-5f) {
    return nearlyEqual(a.x, b.x, epsilon) && nearlyEqual(a.y, b.y, epsilon);
}

} // namespace

TEST_CASE("scene/collision: a wall stops the capsule at its radius", "[scene][collision]") {
    const CollisionWorld world = eastWall();
    const glm::vec3 end = world.moveCapsule(glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), kPlayer);
    CHECK(end.x == Approx(0.7f).margin(1e-5));
    CHECK(end.y == 0.0f);
    CHECK(end.z == Approx(0.0f).margin(1e-6));
    CHECK_FALSE(world.overlaps(end, kPlayer));
    CHECK(world.overlaps(glm::vec3(0.75f, 0.0f, 0.0f), kPlayer));
    // Moving away from the wall is unaffected.
    CHECK(world.moveCapsule(end, glm::vec3(-1.0f, 0.0f, 0.0f), kPlayer).x == Approx(-0.3f));
}

TEST_CASE("scene/collision: the capsule slides along a wall", "[scene][collision]") {
    const CollisionWorld world = eastWall();
    const glm::vec3 end = world.moveCapsule(glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 1.5f), kPlayer);
    CHECK(end.x == Approx(0.7f).margin(1e-5));
    CHECK(end.z == Approx(1.5f).margin(1e-5)); // the tangential component is preserved

    // Pressing into the wall while walking along it keeps the full tangential distance.
    glm::vec3 feet(0.7f, 0.0f, -3.0f);
    for (int frame = 0; frame < 30; ++frame) {
        feet = world.moveCapsule(feet, glm::vec3(0.05f, 0.0f, 0.1f), kPlayer);
    }
    CHECK(feet.x == Approx(0.7f).margin(1e-5));
    CHECK(feet.z == Approx(0.0f).margin(1e-4));
}

TEST_CASE("scene/collision: inside and outside corners", "[scene][collision]") {
    CollisionWorld world;
    world.addBox(Aabb::fromMinMax({2.0f, 0.0f, -5.0f}, {2.2f, 3.0f, 5.0f}), "wall_east");
    world.addBox(Aabb::fromMinMax({-5.0f, 0.0f, -2.2f}, {5.0f, 3.0f, -2.0f}), "wall_north");
    // Walking diagonally into the inside corner ends one radius away from both walls.
    const glm::vec3 corner = world.moveCapsule(glm::vec3(0.0f), glm::vec3(5.0f, 0.0f, -5.0f), kPlayer);
    CHECK(corner.x == Approx(1.7f).margin(1e-4));
    CHECK(corner.z == Approx(-1.7f).margin(1e-4));
    CHECK_FALSE(world.overlaps(corner, kPlayer));

    // Outside corner: sliding along a wall that ends at z = 0 continues past its end without snagging,
    // and a diagonal approach rounds the corner keeping at least one radius from the edge.
    CollisionWorld pillar;
    pillar.addBox(Aabb::fromMinMax({1.0f, 0.0f, -5.0f}, {1.2f, 3.0f, 0.0f}), "wall_segment");
    const glm::vec3 along = pillar.moveCapsule(glm::vec3(0.7f, 0.0f, -2.0f), glm::vec3(0.0f, 0.0f, 3.0f), kPlayer);
    CHECK(along.x == Approx(0.7f).margin(1e-5));
    CHECK(along.z == Approx(1.0f).margin(1e-5));

    glm::vec3 feet(0.6f, 0.0f, -1.0f);
    for (int frame = 0; frame < 40; ++frame) {
        feet = pillar.moveCapsule(feet, glm::vec3(0.05f, 0.0f, 0.05f), kPlayer);
        CHECK_FALSE(pillar.overlaps(feet, kPlayer));
    }
    CHECK(feet.x > 1.2f);          // made it around the end of the wall
    CHECK(feet.z >= 1.0f - 1e-4f); // corner pushes never cost forward progress
}

TEST_CASE("scene/collision: a capsule starting inside a box is pushed out by the minimum separation",
          "[scene][collision]") {
    const CollisionWorld world = eastWall();
    const glm::vec3 shallow(0.8f, 0.0f, 0.0f); // penetrates by 0.1 m
    REQUIRE(world.overlaps(shallow, kPlayer));
    const glm::vec3 pushed = world.moveCapsule(shallow, glm::vec3(0.0f), kPlayer);
    CHECK(pushed.x == Approx(0.7f).margin(1e-5));
    CHECK(pushed.z == Approx(0.0f).margin(1e-6));
    CHECK_FALSE(world.overlaps(pushed, kPlayer));

    // Center inside the rectangle: leave through the nearest face (+Z here: 0.4 m away versus 0.6 and 0.8).
    CollisionWorld block;
    block.addBox(Aabb::fromMinMax({-1.0f, 0.0f, -0.5f}, {1.0f, 1.0f, 0.5f}), "crate");
    const glm::vec3 deep = block.moveCapsule(glm::vec3(0.2f, 0.0f, 0.1f), glm::vec3(0.0f), kPlayer);
    CHECK(deep.x == Approx(0.2f));
    CHECK(deep.z == Approx(0.8f).margin(1e-5));
    CHECK_FALSE(block.overlaps(deep, kPlayer));

    // Near a corner the push is radial.
    const glm::vec3 radial = block.moveCapsule(glm::vec3(1.1f, 0.0f, 0.6f), glm::vec3(0.0f), kPlayer);
    CHECK(glm::length(glm::vec2(radial.x - 1.0f, radial.z - 0.5f)) == Approx(0.3f).margin(1e-5));
    CHECK(radial.x - 1.0f == Approx(radial.z - 0.5f).margin(1e-5)); // along the 45-degree diagonal
}

TEST_CASE("scene/collision: no tunneling through a 2 cm board at sprint speed", "[scene][collision]") {
    CollisionWorld world;
    world.addBox(Aabb::fromMinMax({0.5f, 0.0f, -2.0f}, {0.52f, 2.0f, 2.0f}), "board");

    // 4.5 m/s at dt = 0.1 s is 0.45 m per update, 1.5 times the capsule radius.
    glm::vec3 feet(0.0f);
    for (int frame = 0; frame < 20; ++frame) {
        feet = world.moveCapsule(feet, glm::vec3(0.45f, 0.0f, 0.0f), kPlayer);
    }
    CHECK(feet.x == Approx(0.2f).margin(1e-5));

    // A single long displacement is sub-stepped as well, at any angle.
    CHECK(world.moveCapsule(glm::vec3(0.0f), glm::vec3(5.0f, 0.0f, 0.0f), kPlayer).x == Approx(0.2f).margin(1e-5));
    const glm::vec3 angled = world.moveCapsule(glm::vec3(0.0f), glm::vec3(3.0f, 0.0f, 0.4f), kPlayer);
    CHECK(angled.x == Approx(0.2f).margin(1e-5));
    CHECK(angled.z == Approx(0.4f).margin(1e-5));

    // From the other side as well.
    CHECK(world.moveCapsule(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-0.45f, 0.0f, 0.0f), kPlayer).x ==
          Approx(0.82f).margin(1e-5));
}

TEST_CASE("scene/collision: boxes above the head or below the feet are ignored", "[scene][collision]") {
    CollisionWorld world;
    world.addBox(Aabb::fromMinMax({0.5f, 1.8f, -2.0f}, {0.7f, 2.2f, 2.0f}), "beam");        // above 1.75 m
    world.addBox(Aabb::fromMinMax({0.9f, 1.75f, -2.0f}, {1.1f, 2.2f, 2.0f}), "lintel");     // touches the head
    world.addBox(Aabb::fromMinMax({-10.0f, -0.1f, -10.0f}, {10.0f, 0.0f, 10.0f}), "floor"); // under the feet
    const glm::vec3 end = world.moveCapsule(glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), kPlayer);
    CHECK(end == glm::vec3(2.0f, 0.0f, 0.0f));
    CHECK_FALSE(world.overlaps(glm::vec3(0.6f, 0.0f, 0.0f), kPlayer));

    // A box at head height blocks.
    world.addBox(Aabb::fromMinMax({1.5f, 1.6f, -2.0f}, {1.7f, 2.2f, 2.0f}), "low_beam");
    CHECK(world.moveCapsule(glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), kPlayer).x == Approx(1.2f).margin(1e-5));

    // Standing on a raised platform (feet above a table top) walks over it.
    CollisionWorld table;
    table.addBox(Aabb::fromMinMax({0.5f, 0.0f, -0.5f}, {1.5f, 0.7f, 0.5f}), "table");
    const glm::vec3 over = table.moveCapsule(glm::vec3(0.0f, 0.7f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f), kPlayer);
    CHECK(over == glm::vec3(2.0f, 0.7f, 0.0f));
    // From the floor the table blocks.
    CHECK(table.moveCapsule(glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), kPlayer).x == Approx(0.2f).margin(1e-5));
}

TEST_CASE("scene/collision: vertical displacement passes through and free moves are exact", "[scene][collision]") {
    const CollisionWorld world = eastWall();
    const glm::vec3 feet(-2.0f, 0.0f, 1.0f);
    const glm::vec3 displacement(0.37f, 1.25f, -0.91f);
    CHECK(world.moveCapsule(feet, displacement, kPlayer) == feet + displacement);
    CHECK(CollisionWorld{}.moveCapsule(feet, displacement, kPlayer) == feet + displacement);
}

TEST_CASE("scene/collision: a gap narrower than the capsule cannot be entered", "[scene][collision]") {
    CollisionWorld world;
    world.addBox(Aabb::fromMinMax({-1.0f, 0.0f, -2.0f}, {-0.2f, 2.0f, 0.0f}), "left");
    world.addBox(Aabb::fromMinMax({0.2f, 0.0f, -2.0f}, {1.0f, 2.0f, 0.0f}), "right");
    const glm::vec3 end = world.moveCapsule(glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -3.0f), kPlayer);
    CHECK_FALSE(world.overlaps(end, kPlayer));
    CHECK(end.z >= std::sqrt(0.3f * 0.3f - 0.2f * 0.2f) - 1e-3f); // stopped by both corners of the 0.4 m gap
    CHECK(end.x == Approx(0.0f).margin(0.01));
}

TEST_CASE("scene/collision: results are deterministic", "[scene][collision]") {
    CollisionWorld world;
    world.addBox(Aabb::fromMinMax({2.0f, 0.0f, -5.0f}, {2.2f, 3.0f, 5.0f}));
    world.addBox(Aabb::fromMinMax({-5.0f, 0.0f, -2.2f}, {5.0f, 3.0f, -2.0f}));
    world.addBox(Aabb::fromMinMax({0.4f, 0.0f, -1.0f}, {0.9f, 1.0f, -0.6f}));
    const auto walk = [&world] {
        glm::vec3 feet(0.0f, 0.0f, 1.0f);
        for (int frame = 0; frame < 200; ++frame) {
            const float angle = 0.05f * static_cast<float>(frame);
            feet = world.moveCapsule(feet, glm::vec3(0.08f * std::cos(angle), 0.0f, -0.08f), kPlayer);
        }
        return feet;
    };
    const glm::vec3 first = walk();
    CHECK(walk() == first); // bitwise identical
    CHECK_FALSE(world.overlaps(first, kPlayer));
}

TEST_CASE("scene/collision: box management and invalid input", "[scene][collision]") {
    CollisionWorld world;
    world.addBox(Aabb{}, "empty");
    Aabb corrupted;
    corrupted.min = glm::vec3(0.0f);
    corrupted.max = glm::vec3(kNaN, 1.0f, 1.0f);
    world.addBox(corrupted, "nan");
    CHECK(world.boxes().empty());
    world.addBox(Aabb::fromMinMax({1.0f, 0.0f, -5.0f}, {1.2f, 3.0f, 5.0f}), "wall");
    REQUIRE(world.boxes().size() == 1);
    CHECK(world.boxes()[0].tag == "wall");

    // Non-finite displacement: no movement (but still depenetrates).
    CHECK(world.moveCapsule(glm::vec3(0.8f, 0.0f, 0.0f), glm::vec3(kNaN, 0.0f, 0.0f), kPlayer).x == Approx(0.7f));
    // Non-finite feet come back unchanged and never overlap.
    CHECK(std::isnan(world.moveCapsule(glm::vec3(kNaN), glm::vec3(1.0f), kPlayer).x));
    CHECK_FALSE(world.overlaps(glm::vec3(kNaN), kPlayer));
    // A degenerate capsule is treated as a 1 cm one, so walls still block it.
    CHECK(world.moveCapsule(glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), Capsule{0.0f, 0.0f}).x ==
          Approx(0.99f).margin(1e-5));
    CHECK(world.moveCapsule(glm::vec3(0.0f), glm::vec3(2.0f, 0.0f, 0.0f), Capsule{kNaN, kNaN}).x ==
          Approx(0.7f).margin(1e-5));
    // Absurdly long requests are truncated instead of looping forever.
    const glm::vec3 far = CollisionWorld{}.moveCapsule(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1e9f), kPlayer);
    CHECK(far.z == Approx(-CollisionWorld::kMaxDisplacement));

    world.clear();
    CHECK(world.boxes().empty());
}

TEST_CASE("scene/collision: nearestBlocking reports the closest box at the capsule's height", "[scene][collision]") {
    CollisionWorld world;
    world.addBox(Aabb::fromMinMax({1.0f, 0.0f, -5.0f}, {1.2f, 3.0f, 5.0f}), "wall_east");
    world.addBox(Aabb::fromMinMax({-2.0f, 0.0f, -0.5f}, {-1.5f, 1.0f, 0.5f}), "cabinet");
    world.addBox(Aabb::fromMinMax({-0.5f, 2.0f, -0.5f}, {0.5f, 3.0f, 0.5f}), "beam_overhead");
    world.addBox(Aabb::fromMinMax({-10.0f, -0.1f, -10.0f}, {10.0f, 0.0f, 10.0f}), "floor_slab");

    const auto nearest = world.nearestBlocking(glm::vec3(0.0f), kPlayer);
    REQUIRE(nearest.has_value());
    CHECK(world.boxes()[nearest->index].tag == "wall_east"); // the beam and the floor do not block at this height
    CHECK(nearest->distance == Approx(1.0f));

    // A capsule resting against the wall reports exactly its radius: zero clearance, no penetration.
    const glm::vec3 resting = world.moveCapsule(glm::vec3(0.0f), glm::vec3(3.0f, 0.0f, 0.0f), kPlayer);
    const auto touching = world.nearestBlocking(resting, kPlayer);
    REQUIRE(touching.has_value());
    CHECK(touching->distance == Approx(kPlayer.radius).margin(1e-5));

    // Corners are measured radially; an axis inside a box is at distance 0.
    const auto corner = world.nearestBlocking(glm::vec3(-1.2f, 0.0f, 0.9f), kPlayer);
    REQUIRE(corner.has_value());
    CHECK(world.boxes()[corner->index].tag == "cabinet");
    CHECK(corner->distance == Approx(0.5f)); // (0.3, 0.4) from the corner (-1.5, 0.5)
    const auto inside = world.nearestBlocking(glm::vec3(1.1f, 0.0f, 0.0f), kPlayer);
    REQUIRE(inside.has_value());
    CHECK(inside->distance == 0.0f);

    // Ties keep the earliest box; nothing blocks above everything; invalid input gives nothing.
    CollisionWorld twins;
    twins.addBox(Aabb::fromMinMax({1.0f, 0.0f, -1.0f}, {2.0f, 2.0f, 1.0f}), "first");
    twins.addBox(Aabb::fromMinMax({-2.0f, 0.0f, -1.0f}, {-1.0f, 2.0f, 1.0f}), "second");
    const auto tie = twins.nearestBlocking(glm::vec3(0.0f), kPlayer);
    REQUIRE(tie.has_value());
    CHECK(tie->index == 0);
    CHECK_FALSE(world.nearestBlocking(glm::vec3(0.0f, 10.0f, 0.0f), kPlayer).has_value());
    CHECK_FALSE(world.nearestBlocking(glm::vec3(kNaN, 0.0f, 0.0f), kPlayer).has_value());
    CHECK_FALSE(CollisionWorld{}.nearestBlocking(glm::vec3(0.0f), kPlayer).has_value());
}

TEST_CASE("scene/collision: a box turned about +Y blocks with its own footprint", "[scene][collision]") {
    // A 2 m board, 0.2 m thick, turned by 45 degrees: its world box is a 1.56 m square around it.
    const Aabb board = Aabb::fromMinMax({-1.0f, 0.0f, -0.1f}, {1.0f, 2.0f, 0.1f});
    const glm::mat4 turn = glm::rotate(glm::mat4(1.0f), degreesToRadians(45.0f), kUp);
    CollisionWorld world;
    world.addBox(board, turn, "board");
    REQUIRE(world.boxes().size() == 1);
    const rv::scene::ColliderBox& collider = world.boxes()[0];
    CHECK(collider.box == board.transformed(turn));
    REQUIRE(collider.turned.has_value());
    const glm::vec2 axisX = collider.turned->axisX;
    const glm::vec2 axisZ = collider.turned->axisZ;
    CHECK(nearlyEqual2(axisX, glm::vec2(std::sqrt(0.5f), -std::sqrt(0.5f))));
    CHECK(nearlyEqual2(axisZ, glm::vec2(std::sqrt(0.5f), std::sqrt(0.5f))));
    // A point or a move expressed along the board's own axes, in world space.
    const auto along = [&](float x, float z) {
        const glm::vec2 point = x * axisX + z * axisZ;
        return glm::vec3(point.x, 0.0f, point.y);
    };

    // Inside the world box but 0.75 m from the board: free, where the world box alone would block.
    CHECK_FALSE(world.overlaps(along(0.0f, 0.85f), kPlayer));
    CollisionWorld worldBox;
    worldBox.addBox(board.transformed(turn), "board");
    CHECK(worldBox.overlaps(along(0.0f, 0.85f), kPlayer));
    const auto nearest = world.nearestBlocking(along(0.0f, 0.85f), kPlayer);
    REQUIRE(nearest.has_value());
    CHECK(nearest->distance == Approx(0.75f).margin(1e-5));

    // Walking straight at the board stops one radius before its face; at an angle the capsule slides along it.
    CHECK(nearlyEqual(world.moveCapsule(along(0.0f, 2.0f), along(0.0f, -3.0f), kPlayer), along(0.0f, 0.4f), 1e-4f));
    const glm::vec3 slid = world.moveCapsule(along(0.0f, 2.0f), along(0.5f, -3.0f), kPlayer);
    CHECK(nearlyEqual(slid, along(0.5f, 0.4f), 1e-4f));
    CHECK_FALSE(world.overlaps(slid, kPlayer));
    const auto resting = world.nearestBlocking(slid, kPlayer);
    REQUIRE(resting.has_value());
    CHECK(resting->distance == Approx(kPlayer.radius).margin(1e-4));
    // Around the end of the board the push is radial; from inside, the capsule leaves through the nearest face.
    const glm::vec3 rounded = world.moveCapsule(along(1.1f, 0.15f), glm::vec3(0.0f), kPlayer);
    CHECK(glm::length(glm::vec2(rounded.x, rounded.z) - glm::vec2(along(1.0f, 0.1f).x, along(1.0f, 0.1f).z)) ==
          Approx(kPlayer.radius).margin(1e-4));
    CHECK(nearlyEqual(world.moveCapsule(along(0.3f, 0.05f), glm::vec3(0.0f), kPlayer), along(0.3f, 0.4f), 1e-4f));

    // No tunneling through it at sprint speed either.
    glm::vec3 feet = along(0.0f, 1.0f);
    for (int frame = 0; frame < 10; ++frame) {
        feet = world.moveCapsule(feet, along(0.0f, -0.45f), kPlayer);
    }
    CHECK(nearlyEqual(feet, along(0.0f, 0.4f), 1e-4f));
}

TEST_CASE("scene/collision: placed boxes are exact when upright, conservative when tilted", "[scene][collision]") {
    const Aabb crate = Aabb::fromMinMax({-0.5f, 0.0f, -0.25f}, {0.5f, 1.0f, 0.25f});
    const glm::mat4 identity(1.0f);
    const glm::mat4 turned = glm::translate(identity, glm::vec3(2.0f, 0.0f, 3.0f)) *
                             glm::rotate(identity, degreesToRadians(30.0f), kUp) *
                             glm::scale(identity, glm::vec3(2.0f, 1.0f, 0.5f));
    const glm::mat4 tilted = glm::rotate(identity, degreesToRadians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    CollisionWorld world;
    world.addBox(crate, identity, "plain");
    world.addBox(crate, glm::rotate(identity, degreesToRadians(90.0f), kUp), "quarter");
    world.addBox(crate, turned, "turned");
    world.addBox(crate, tilted, "tilted");
    REQUIRE(world.boxes().size() == 4);

    // Unturned and quarter-turned boxes are exact as world boxes.
    CHECK(world.boxes()[0].box == crate);
    CHECK_FALSE(world.boxes()[0].turned.has_value());
    CHECK(nearlyEqual(world.boxes()[1].box.min, glm::vec3(-0.25f, 0.0f, -0.5f), 1e-6f));
    CHECK(nearlyEqual(world.boxes()[1].box.max, glm::vec3(0.25f, 1.0f, 0.5f), 1e-6f));
    CHECK_FALSE(world.boxes()[1].turned.has_value());

    // Any other turn keeps the exact footprint, with the translation and the scale.
    const rv::scene::ColliderBox& placed = world.boxes()[2];
    CHECK(placed.tag == "turned");
    CHECK(placed.box == crate.transformed(turned));
    REQUIRE(placed.turned.has_value());
    CHECK(nearlyEqual2(placed.turned->center, glm::vec2(2.0f, 3.0f)));
    CHECK(nearlyEqual2(placed.turned->halfExtents, glm::vec2(1.0f, 0.125f)));
    const float cos30 = std::cos(degreesToRadians(30.0f));
    CHECK(nearlyEqual2(placed.turned->axisX, glm::vec2(cos30, -0.5f)));
    CHECK(nearlyEqual2(placed.turned->axisZ, glm::vec2(0.5f, cos30)));

    // A tilted box falls back to its world box.
    CHECK(world.boxes()[3].box == crate.transformed(tilted));
    CHECK_FALSE(world.boxes()[3].turned.has_value());

    // Invalid input adds nothing.
    CollisionWorld invalid;
    invalid.addBox(Aabb{}, identity, "empty");
    invalid.addBox(crate, glm::translate(identity, glm::vec3(kNaN)), "nan");
    Aabb corrupted = crate;
    corrupted.max.x = kNaN;
    invalid.addBox(corrupted, identity, "corrupted");
    CHECK(invalid.boxes().empty());
}
