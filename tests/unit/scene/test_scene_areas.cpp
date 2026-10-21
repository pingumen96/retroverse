// The areas of a scene (rv/scene/scene_areas.hpp, I4 step 3, ADR-0013 §9): the area that holds the eye (the first
// holding it, else the last one held), the lights a view packs there (the area's own, its key light first, the nearest
// to the eye up to the capacity, in the scene's order), and the areas read from a scene file.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/scene/scene_areas.hpp>
#include <rv/scene/scene_description.hpp>

using rv::scene::AreaLighting;
using rv::scene::Light;
using rv::scene::LightType;
using rv::scene::SceneArea;

namespace {

SceneArea area(std::string id, glm::vec3 min, glm::vec3 max) {
    SceneArea result;
    result.id = std::move(id);
    result.boxes.push_back(rv::core::Aabb::fromMinMax(min, max));
    return result;
}

Light pointLight(std::string name, std::string areaId, glm::vec3 position) {
    Light light;
    light.name = std::move(name);
    light.type = LightType::Point;
    light.position = position;
    light.area = std::move(areaId);
    return light;
}

/// Two rooms side by side and a hall south of both: room A x in [-10, 0], room B x in [0, 10], z in [-10, 0]; the hall
/// z in [0, 3] with a passage into A at x in [-6, -4].
std::vector<SceneArea> floorAreas() {
    SceneArea roomA = area("room:a", {-10.0f, 0.0f, -10.0f}, {0.0f, 3.0f, 0.0f});
    roomA.boxes.push_back(rv::core::Aabb::fromMinMax({-6.0f, 0.0f, 0.0f}, {-4.0f, 3.0f, 0.2f}));
    return {roomA, area("room:b", {0.0f, 0.0f, -10.0f}, {10.0f, 3.0f, 0.0f}),
            area("hall", {-10.0f, 0.0f, 0.2f}, {10.0f, 3.0f, 3.0f})};
}

} // namespace

TEST_CASE("scene/areas: the area holding a point, the nearest one, the lights of each", "[scene][areas]") {
    const std::vector<SceneArea> areas = floorAreas();
    CHECK(rv::scene::areaAt(areas, {-5.0f, 1.6f, -5.0f}) == 0u);
    CHECK(rv::scene::areaAt(areas, {-5.0f, 1.6f, 0.1f}) == 0u); // the passage belongs to its room
    CHECK(rv::scene::areaAt(areas, {5.0f, 1.6f, -5.0f}) == 1u);
    CHECK(rv::scene::areaAt(areas, {5.0f, 1.6f, 1.0f}) == 2u);
    CHECK_FALSE(rv::scene::areaAt(areas, {5.0f, 1.6f, 0.1f}).has_value()); // in a wall
    CHECK(rv::scene::areaAt(areas, {0.0f, 1.6f, -5.0f}) == 0u);             // on a shared face: the first area
    CHECK(rv::scene::nearestArea(areas, {5.0f, 1.6f, 0.15f}) == 2u);
    CHECK(rv::scene::nearestArea(areas, {5.0f, 1.6f, 0.05f}) == 1u);
    CHECK(rv::scene::nearestArea(areas, {-30.0f, 1.6f, -5.0f}) == 0u);
    CHECK_FALSE(rv::scene::nearestArea({}, {0.0f, 0.0f, 0.0f}).has_value());

    const std::vector<Light> lights{pointLight("a1", "room:a", {-5, 2, -5}), pointLight("h1", "hall", {0, 2, 1}),
                                    pointLight("b1", "room:b", {5, 2, -5}), pointLight("a2", "room:a", {-2, 2, -2}),
                                    pointLight("x", "elsewhere", {0, 2, 0})};
    const auto byArea = rv::scene::lightsByArea(lights, areas);
    REQUIRE(byArea.size() == 3);
    CHECK(byArea[0] == std::vector<std::uint32_t>{0, 3});
    CHECK(byArea[1] == std::vector<std::uint32_t>{2});
    CHECK(byArea[2] == std::vector<std::uint32_t>{1});
}

TEST_CASE("scene/areas: the eye's area with hysteresis, and its lights only", "[scene][areas]") {
    const std::vector<SceneArea> areas = floorAreas();
    std::vector<Light> lights{pointLight("a1", "room:a", {-5, 2, -5}), pointLight("h1", "hall", {0, 2, 1}),
                              pointLight("b1", "room:b", {5, 2, -5}), pointLight("a2", "room:a", {-2, 2, -2})};
    AreaLighting lighting(areas, lights, {-5.0f, 1.65f, 2.0f}, 16);
    CHECK_FALSE(lighting.empty());
    CHECK(lighting.area() == 2u);
    CHECK(std::vector<std::uint32_t>(lighting.selection().begin(), lighting.selection().end()) ==
          std::vector<std::uint32_t>{1});

    // Into the passage, then the room: room A's lights, in the scene's order.
    CHECK(lighting.update(areas, lights, {-5.0f, 1.65f, 0.1f}));
    CHECK(lighting.area() == 0u);
    CHECK(std::vector<std::uint32_t>(lighting.selection().begin(), lighting.selection().end()) ==
          std::vector<std::uint32_t>{0, 3});
    CHECK_FALSE(lighting.update(areas, lights, {-5.0f, 1.65f, -3.0f})); // still room A: nothing changes
    // In a wall (no area): the last area held.
    CHECK_FALSE(lighting.update(areas, lights, {5.0f, 1.65f, 0.1f}));
    CHECK(lighting.area() == 0u);
    CHECK(lighting.update(areas, lights, {5.0f, 1.65f, -1.0f}));
    CHECK(lighting.area() == 1u);
    CHECK(std::vector<std::uint32_t>(lighting.selection().begin(), lighting.selection().end()) ==
          std::vector<std::uint32_t>{2});

    // An edit of the lights (the overlay moving one to another area's list is not possible: only flags and places).
    lights[2].castsShadows = true;
    CHECK_FALSE(lighting.reselect(lights, {5.0f, 1.65f, -1.0f}));

    // Without areas: nothing is selected and nothing changes.
    AreaLighting none;
    CHECK(none.empty());
    CHECK_FALSE(none.update({}, lights, {0.0f, 0.0f, 0.0f}));
    CHECK(none.selection().empty());
    CHECK_FALSE(none.area().has_value());
}

TEST_CASE("scene/areas: a hall of many lights packs its key light and those nearest the eye", "[scene][areas]") {
    const std::vector<SceneArea> areas{area("hall", {0.0f, 0.0f, -1.5f}, {200.0f, 3.2f, 1.5f})};
    std::vector<Light> lights;
    for (int i = 0; i < 30; ++i) {
        lights.push_back(pointLight("h" + std::to_string(i), "hall", {3.0f + 6.0f * static_cast<float>(i), 2.9f, 0.0f}));
    }
    // A spot far down the hall casts the area's shadow: it is always packed, first among equals.
    lights[25].type = LightType::Spot;
    lights[25].castsShadows = true;
    lights[27].castsShadows = true; // a later one is no key light
    Light sun;
    sun.name = "sun";
    sun.type = LightType::Directional;
    sun.area = "hall";
    lights.push_back(sun);

    AreaLighting lighting(areas, lights, {1.0f, 1.65f, 0.0f}, 16);
    std::vector<std::uint32_t> expected{25, 30};
    for (std::uint32_t i = 0; i < 15; ++i) {
        expected.push_back(i);
    }
    std::sort(expected.begin(), expected.end());
    CHECK(std::vector<std::uint32_t>(lighting.selection().begin(), lighting.selection().end()) == expected);

    // Walking east, the set follows the eye: at x = 90 the 15 nearest are those around it.
    CHECK(lighting.update(areas, lights, {90.0f, 1.65f, 0.0f}));
    std::vector<std::uint32_t> around{25, 30};
    for (std::uint32_t i = 7; i <= 21; ++i) {
        around.push_back(i);
    }
    std::sort(around.begin(), around.end());
    CHECK(std::vector<std::uint32_t>(lighting.selection().begin(), lighting.selection().end()) == around);
    // A step that keeps the nearest set changes nothing.
    CHECK_FALSE(lighting.update(areas, lights, {89.5f, 1.65f, 0.0f}));

    // The work per frame is a pass over the area's lights: microseconds (a loose bound for any build).
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < 1000; ++frame) {
        (void)lighting.update(areas, lights, {static_cast<float>(frame % 180), 1.65f, 0.0f});
    }
    const auto perFrame =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 1000.0;
    INFO("per frame: " << perFrame << " us");
    CHECK(perFrame < 200.0);
}

TEST_CASE("scene/areas: a scene file's areas and its lights' areas", "[scene][areas]") {
    const nlohmann::json document = nlohmann::json::parse(R"({
        "schemaVersion": 1, "name": "two areas",
        "areas": [{"id": "west", "boxes": [{"min": [-5, 0, -5], "max": [0, 3, 5]}]},
                  {"id": "east", "boxes": [{"min": [0, 0, -5], "max": [5, 3, 5]}, {"min": [5, 0, -1], "max": [6, 3, 1]}]}],
        "lights": [{"type": "point", "position": [-2, 2, 0], "area": "west"},
                   {"type": "point", "position": [2, 2, 0], "area": "east", "name": "e"}]})");
    const auto scene = rv::scene::parseSceneDescription(document, "two.json");
    REQUIRE(scene.has_value());
    REQUIRE(scene->areas.size() == 2);
    CHECK(scene->areas[1].id == "east");
    CHECK(scene->areas[1].boxes.size() == 2);
    CHECK(scene->areas[1].contains({5.5f, 1.0f, 0.0f}));
    CHECK_FALSE(scene->areas[0].contains({5.5f, 1.0f, 0.0f}));
    CHECK(scene->findArea("east") == 1u);
    CHECK_FALSE(scene->findArea("north").has_value());
    CHECK(scene->lights[0].area == "west");
    CHECK(scene->lights[1].area == "east");

    // A scene without areas reads as before: no area, lights without one.
    const auto plain = rv::scene::parseSceneDescription(
        nlohmann::json::parse(R"({"schemaVersion": 1, "name": "plain", "lights": [{"type": "point", "position": [0, 2, 0]}]})"),
        "plain.json");
    REQUIRE(plain.has_value());
    CHECK(plain->areas.empty());
    CHECK(plain->lights[0].area.empty());
}
