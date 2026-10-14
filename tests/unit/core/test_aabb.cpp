#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/aabb.hpp>

using Catch::Approx;
using rv::core::Aabb;

TEST_CASE("core/aabb: empty box is the identity of expand", "[core][aabb]") {
    Aabb box;
    CHECK(box.isEmpty());
    CHECK(box.size() == glm::vec3(0.0f));
    CHECK_FALSE(box.contains(glm::vec3(0.0f)));

    box.expand(glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK_FALSE(box.isEmpty());
    CHECK(box.min == glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(box.max == glm::vec3(1.0f, 2.0f, 3.0f));

    Aabb other;
    box.expand(other); // expanding by an empty box changes nothing
    CHECK(box.min == glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST_CASE("core/aabb: construction helpers", "[core][aabb]") {
    const Aabb swapped = Aabb::fromMinMax({1.0f, 1.0f, 1.0f}, {-1.0f, -2.0f, -3.0f});
    CHECK(swapped.min == glm::vec3(-1.0f, -2.0f, -3.0f));
    CHECK(swapped.max == glm::vec3(1.0f, 1.0f, 1.0f));

    const Aabb centered = Aabb::fromCenterHalfExtents({0.0f, 1.0f, 0.0f}, {0.5f, 1.0f, 0.25f});
    CHECK(centered.size() == glm::vec3(1.0f, 2.0f, 0.5f));
    CHECK(centered.center() == glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(centered.halfExtents() == glm::vec3(0.5f, 1.0f, 0.25f));

    const std::array<glm::vec3, 3> points{glm::vec3(0.0f), glm::vec3(2.0f, -1.0f, 0.5f), glm::vec3(-1.0f, 3.0f, 0.0f)};
    const Aabb fromPoints = Aabb::fromPoints(points);
    CHECK(fromPoints.min == glm::vec3(-1.0f, -1.0f, 0.0f));
    CHECK(fromPoints.max == glm::vec3(2.0f, 3.0f, 0.5f));
}

TEST_CASE("core/aabb: containment, intersection, inflation", "[core][aabb]") {
    const Aabb a = Aabb::fromMinMax({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    const Aabb b = Aabb::fromMinMax({1.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 1.0f}); // touches a on x = 1
    const Aabb c = Aabb::fromMinMax({1.5f, 1.5f, 1.5f}, {2.0f, 2.0f, 2.0f});

    CHECK(a.contains(glm::vec3(1.0f, 1.0f, 1.0f)));
    CHECK_FALSE(a.contains(glm::vec3(1.0f, 1.0f, 1.0001f)));
    CHECK(a.intersects(b));
    CHECK_FALSE(a.intersects(c));
    CHECK_FALSE(a.intersects(Aabb{}));
    CHECK(a.merged(c).contains(c));
    CHECK(a.inflated(0.5f).intersects(c));
    CHECK(a.inflated(0.1f).contains(a));
}

TEST_CASE("core/aabb: transformed box (Arvo)", "[core][aabb]") {
    const Aabb unit = Aabb::fromMinMax({-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f});

    const glm::mat4 translate = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, -2.0f));
    const Aabb moved = unit.transformed(translate);
    CHECK(moved.center() == glm::vec3(10.0f, 0.0f, -2.0f));
    CHECK(moved.size() == glm::vec3(1.0f));

    const glm::mat4 rotate = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const Aabb rotated = unit.transformed(rotate);
    CHECK(rotated.size().x == Approx(std::sqrt(2.0f)).margin(1e-5));
    CHECK(rotated.size().y == Approx(1.0f).margin(1e-5));
    CHECK(rotated.size().z == Approx(std::sqrt(2.0f)).margin(1e-5));

    const glm::mat4 scale = glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 3.0f, -1.0f));
    const Aabb scaled = unit.transformed(scale);
    CHECK(scaled.size() == glm::vec3(2.0f, 3.0f, 1.0f));
}

TEST_CASE("core/aabb: ray intersection (slab test)", "[core][aabb]") {
    const Aabb box = Aabb::fromMinMax({-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f});

    const auto hit = box.intersectRay({{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}});
    REQUIRE(hit.has_value());
    CHECK(*hit == Approx(4.0f));

    const auto inside = box.intersectRay({{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}});
    REQUIRE(inside.has_value());
    CHECK(*inside == Approx(0.0f));

    CHECK_FALSE(box.intersectRay({{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 1.0f}}).has_value());   // pointing away
    CHECK_FALSE(box.intersectRay({{0.0f, 2.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}).has_value());  // parallel, outside
    CHECK_FALSE(box.intersectRay({{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, 3.0f).has_value()); // beyond tMax
}
