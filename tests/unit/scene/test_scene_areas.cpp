// The areas of a scene (rv/scene/scene_areas.hpp, I4 step 3, ADR-0013 §9; I6 step 1, ADR-0014 §4): the area that
// holds the eye (the first holding it, else the last one held), the lights a view packs there (the area's own, its key
// light first, the nearest to the eye up to the capacity, in the scene's order; then a few of each neighbour's near
// the doorway they share, never the key light; none wholly behind the eye), the environment eased between areas, and
// the areas read from a scene file.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
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

/// Two rooms side by side, a wall apart, and a hall south of both: room A x in [-10, 0], room B x in [0.2, 10], z in
/// [-10, 0]; the hall z in [0.2, 3] with a passage into A at x in [-6, -4] (room B has no doorway).
std::vector<SceneArea> floorAreas() {
    SceneArea roomA = area("room:a", {-10.0f, 0.0f, -10.0f}, {0.0f, 3.0f, 0.0f});
    roomA.boxes.push_back(rv::core::Aabb::fromMinMax({-6.0f, 0.0f, 0.0f}, {-4.0f, 3.0f, 0.2f}));
    return {roomA, area("room:b", {0.2f, 0.0f, -10.0f}, {10.0f, 3.0f, 0.0f}),
            area("hall", {-10.0f, 0.0f, 0.2f}, {10.0f, 3.0f, 3.0f})};
}

std::vector<std::uint32_t> selected(const AreaLighting& lighting) {
    return {lighting.selection().begin(), lighting.selection().end()};
}

} // namespace

TEST_CASE("scene/areas: the area holding a point, the nearest one, the lights of each", "[scene][areas]") {
    const std::vector<SceneArea> areas = floorAreas();
    CHECK(rv::scene::areaAt(areas, {-5.0f, 1.6f, -5.0f}) == 0u);
    CHECK(rv::scene::areaAt(areas, {-5.0f, 1.6f, 0.1f}) == 0u); // the passage belongs to its room
    CHECK(rv::scene::areaAt(areas, {5.0f, 1.6f, -5.0f}) == 1u);
    CHECK(rv::scene::areaAt(areas, {5.0f, 1.6f, 1.0f}) == 2u);
    CHECK_FALSE(rv::scene::areaAt(areas, {5.0f, 1.6f, 0.1f}).has_value()); // in a wall
    CHECK(rv::scene::areaAt(areas, {0.0f, 1.6f, -5.0f}) == 0u);             // on room A's face
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

TEST_CASE("scene/areas: the neighbours are the areas whose boxes touch over a face, their portals the common faces",
          "[scene][areas]") {
    const std::vector<SceneArea> areas = floorAreas();
    const auto neighbours = rv::scene::areaNeighbours(areas);
    REQUIRE(neighbours.size() == 3);
    // Room A and the hall through A's passage; room B touches nothing (a wall apart from A, no doorway).
    REQUIRE(neighbours[0].size() == 1);
    CHECK(neighbours[0][0].area == 2u);
    REQUIRE(neighbours[0][0].portals.size() == 1);
    CHECK(neighbours[0][0].portals[0] == rv::core::Aabb::fromMinMax({-6.0f, 0.0f, 0.2f}, {-4.0f, 3.0f, 0.2f}));
    CHECK(neighbours[1].empty());
    REQUIRE(neighbours[2].size() == 1);
    CHECK(neighbours[2][0].area == 0u);
    CHECK(neighbours[2][0].portals == neighbours[0][0].portals);

    // Boxes meeting along an edge or at a corner only are no neighbours; overlapping ones are.
    const std::vector<SceneArea> edge{area("a", {0, 0, 0}, {1, 1, 1}), area("b", {1, 1, 0}, {2, 2, 1})};
    CHECK(rv::scene::areaNeighbours(edge)[0].empty());
    const std::vector<SceneArea> corner{area("a", {0, 0, 0}, {1, 1, 1}), area("b", {1, 1, 1}, {2, 2, 2})};
    CHECK(rv::scene::areaNeighbours(corner)[0].empty());
    const std::vector<SceneArea> overlap{area("a", {0, 0, 0}, {1, 1, 1}), area("b", {0.5f, 0, 0}, {2, 1, 1})};
    REQUIRE(rv::scene::areaNeighbours(overlap)[0].size() == 1);
    CHECK(rv::scene::areaNeighbours(overlap)[0][0].portals[0] ==
          rv::core::Aabb::fromMinMax({0.5f, 0, 0}, {1, 1, 1}));
}

TEST_CASE("scene/areas: the eye's area with hysteresis, its own lights, then its neighbours' near the doorway",
          "[scene][areas]") {
    const std::vector<SceneArea> areas = floorAreas();
    std::vector<Light> lights{pointLight("a1", "room:a", {-5, 2, -5}), pointLight("h1", "hall", {0, 2, 1}),
                              pointLight("b1", "room:b", {5, 2, -5}), pointLight("a2", "room:a", {-2, 2, -2})};
    AreaLighting lighting(areas, lights, {-5.0f, 1.65f, 2.0f}, 16);
    CHECK_FALSE(lighting.empty());
    CHECK(lighting.area() == 2u);
    // The hall's own light, then room A's two (5.2 m and 3 m from the passage's face): never room B's, a wall apart.
    CHECK(selected(lighting) == std::vector<std::uint32_t>{1, 0, 3});
    CHECK(lighting.ownCount() == 1);

    // Into the passage, then the room: room A's lights, in the scene's order, then the hall's (5.2 m from the face).
    CHECK(lighting.update(areas, lights, {-5.0f, 1.65f, 0.1f}));
    CHECK(lighting.area() == 0u);
    CHECK(selected(lighting) == std::vector<std::uint32_t>{0, 3, 1});
    CHECK(lighting.ownCount() == 2);
    CHECK_FALSE(lighting.update(areas, lights, {-5.0f, 1.65f, -3.0f})); // still room A: nothing changes
    // In a wall (no area): the last area held.
    CHECK_FALSE(lighting.update(areas, lights, {5.0f, 1.65f, 0.1f}));
    CHECK(lighting.area() == 0u);
    CHECK(lighting.update(areas, lights, {5.0f, 1.65f, -1.0f}));
    CHECK(lighting.area() == 1u);
    CHECK(selected(lighting) == std::vector<std::uint32_t>{2});
    CHECK(lighting.ownCount() == 1);

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

TEST_CASE("scene/areas: a neighbour's lights: at most four, near the portal and reaching it, never the key light",
          "[scene][areas]") {
    // A room north of a hall, its doorway's passage at x in [-1, 1], z in [-0.2, 0].
    SceneArea room = area("room", {-5.0f, 0.0f, -10.0f}, {5.0f, 3.2f, -0.2f});
    room.boxes.push_back(rv::core::Aabb::fromMinMax({-1.0f, 0.0f, -0.2f}, {1.0f, 3.2f, 0.0f}));
    const std::vector<SceneArea> areas{room, area("hall", {-20.0f, 0.0f, 0.0f}, {20.0f, 3.2f, 3.0f})};
    std::vector<Light> lights;
    // The room's key spot by the doorway, six lights at 1 m to 6 m from the portal, one far in, one that cannot reach.
    Light key = pointLight("key", "room", {0.0f, 2.9f, -1.0f});
    key.type = LightType::Spot;
    key.castsShadows = true;
    lights.push_back(key);
    for (int i = 1; i <= 6; ++i) {
        lights.push_back(pointLight("r" + std::to_string(i), "room", {0.0f, 1.5f, -static_cast<float>(i) - 0.5f}));
    }
    lights.push_back(pointLight("far", "room", {0.0f, 1.5f, -9.0f}));
    Light weak = pointLight("weak", "room", {0.0f, 1.5f, -2.0f});
    weak.range = 1.5f; // 2 m from the portal: it cannot shine through
    lights.push_back(weak);
    lights.push_back(pointLight("hall", "hall", {0.0f, 2.9f, 1.5f}));

    AreaLighting lighting(areas, lights, {0.0f, 1.65f, 2.0f}, 16);
    REQUIRE(lighting.area() == 1u);
    // The hall's own light, then the room's four nearest the portal (the key, 1 m away, among them): never the far
    // one (9 m), nor the weak one, whose range ends before the portal.
    CHECK(lighting.ownCount() == 1);
    CHECK(selected(lighting) == std::vector<std::uint32_t>{9, 0, 1, 2, 3});
    // The room's key light is no key light from the hall: packLights' keyCandidates (ownCount()) leave it out.

    // At most the capacity in all: with room for two, the two of the four nearest the eye.
    AreaLighting tight(areas, lights, {0.0f, 1.65f, 2.0f}, 3);
    CHECK(tight.ownCount() == 1);
    CHECK(selected(tight) == std::vector<std::uint32_t>{9, 0, 1});
    AreaLighting full(areas, lights, {0.0f, 1.65f, 2.0f}, 1);
    CHECK(selected(full) == std::vector<std::uint32_t>{9});

    // From the room, its own lights first (its key light among them), the hall's light after them.
    CHECK(lighting.update(areas, lights, {0.0f, 1.65f, -5.0f}));
    CHECK(lighting.area() == 0u);
    CHECK(lighting.ownCount() == 9);
    CHECK(selected(lighting) == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
}

TEST_CASE("scene/areas: the lights wholly behind the eye are left out, the key light kept", "[scene][areas]") {
    const std::vector<SceneArea> areas{area("room", {-10.0f, 0.0f, -10.0f}, {10.0f, 3.2f, 10.0f})};
    Light key = pointLight("key", "room", {0.0f, 2.9f, 8.0f});
    key.type = LightType::Spot;
    key.castsShadows = true;
    key.range = 4.0f;
    std::vector<Light> lights{key, pointLight("behind", "room", {0.0f, 2.0f, 6.0f}),
                              pointLight("ahead", "room", {0.0f, 2.0f, -6.0f}),
                              pointLight("beside", "room", {6.0f, 2.0f, 0.0f})};
    lights[1].range = 5.0f; // its sphere ends 1 m behind the eye
    lights[2].range = 5.0f;
    lights[3].range = 5.0f;
    AreaLighting lighting(areas, lights, {0.0f, 1.65f, 0.0f}, 16);
    CHECK(selected(lighting) == std::vector<std::uint32_t>{0, 1, 2, 3}); // no direction: every light
    // Looking north (-z): the light 6 m south with a 5 m range lights nothing in view; the key light stays.
    CHECK(lighting.update(areas, lights, {0.0f, 1.65f, 0.0f}, {0.0f, 0.0f, -1.0f}));
    CHECK(selected(lighting) == std::vector<std::uint32_t>{0, 2, 3});
    // A range that reaches past the eye keeps it.
    lights[1].range = 6.5f;
    CHECK(lighting.reselect(lights, {0.0f, 1.65f, 0.0f}, {0.0f, 0.0f, -1.0f}));
    CHECK(selected(lighting) == std::vector<std::uint32_t>{0, 1, 2, 3});
    // Looking south: the light ahead (north) is behind now.
    CHECK(lighting.reselect(lights, {0.0f, 1.65f, 0.0f}, {0.0f, 0.0f, 1.0f}));
    CHECK(selected(lighting) == std::vector<std::uint32_t>{0, 1, 3});
}

TEST_CASE("scene/areas: an area's environment over the scene's, eased between areas", "[scene][areas]") {
    rv::scene::EnvironmentSetup scene;
    scene.exposure = 1.5f;
    scene.ambientIntensity = 0.3f;
    scene.fillIntensity = 0.2f;
    rv::scene::AreaEnvironment hallTerms;
    hallTerms.exposure = 2.0f;
    hallTerms.fillIntensity = 0.4f;
    const rv::scene::EnvironmentSetup hall = hallTerms.over(scene);
    CHECK(hall.exposure == 2.0f);
    CHECK(hall.fillIntensity == 0.4f);
    CHECK(hall.ambientIntensity == 0.3f); // the scene's where the area sets nothing
    CHECK(rv::scene::AreaEnvironment{}.over(scene).exposure == 1.5f);
    CHECK_FALSE(rv::scene::sameAreaTerms(scene, hall));

    // From the room's environment to the hall's in half a second, smoothly, the same at every fixed step.
    rv::scene::EnvironmentEase ease(scene);
    CHECK_FALSE(ease.easing());
    ease.retarget(hall, false);
    CHECK(ease.easing());
    CHECK(ease.current().exposure == 1.5f);
    float previous = ease.current().exposure;
    int frames = 0;
    while (ease.advance(1.0f / 60.0f)) {
        CHECK(ease.current().exposure >= previous);
        previous = ease.current().exposure;
        ++frames;
    }
    CHECK(frames >= 30); // 0.5 s at 60 frames a second (31 when the sum of the steps rounds below it)
    CHECK(frames <= 31);
    CHECK(ease.current().exposure == 2.0f);
    CHECK(ease.current().fillIntensity == 0.4f);
    CHECK_FALSE(ease.easing());
    // Half way, half way (smoothstep's middle).
    ease.retarget(scene, false);
    for (int i = 0; i < 15; ++i) {
        (void)ease.advance(1.0f / 60.0f);
    }
    CHECK(std::abs(ease.current().exposure - 1.75f) < 1e-4f);
    // Turned back half way: from where it stands.
    ease.retarget(hall, false);
    CHECK(std::abs(ease.current().exposure - 1.75f) < 1e-4f);
    // reduceMotion: at once.
    ease.retarget(scene, true);
    CHECK_FALSE(ease.easing());
    CHECK(ease.current().exposure == 1.5f);
    CHECK_FALSE(ease.advance(1.0f));
    // Bad times move nothing.
    ease.retarget(hall, false);
    CHECK_FALSE(ease.advance(-1.0f));
    CHECK_FALSE(ease.advance(std::numeric_limits<float>::quiet_NaN()));
    CHECK(ease.current().exposure == 1.5f);
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
    CHECK(scene->areas[0].environment == rv::scene::AreaEnvironment{});

    // An area's own environment (I6 step 1): each term optional, unknown or negative ones refused.
    const auto lit = rv::scene::parseSceneDescription(nlohmann::json::parse(R"({
        "schemaVersion": 1, "name": "lit",
        "environment": {"fillColor": "#FFFFFF", "fillIntensity": 0.1},
        "areas": [{"id": "hall", "boxes": [{"min": [0, 0, 0], "max": [1, 1, 1]}],
                   "environment": {"exposure": 2.0, "ambientGround": "#808080", "fillIntensity": 0.3}}]})"),
                                                      "lit.json");
    REQUIRE(lit.has_value());
    CHECK(lit->environment.fillIntensity == 0.1f);
    const rv::scene::AreaEnvironment& terms = lit->areas[0].environment;
    CHECK(terms.exposure == 2.0f);
    CHECK(terms.fillIntensity == 0.3f);
    REQUIRE(terms.ambientGround.has_value());
    CHECK(std::abs(terms.ambientGround->g - 0.2158605f) < 1e-5f);
    CHECK_FALSE(terms.ambientSky.has_value());
    CHECK_FALSE(rv::scene::parseSceneDescription(nlohmann::json::parse(R"({"schemaVersion": 1, "name": "x",
        "areas": [{"id": "a", "boxes": [{"min": [0, 0, 0], "max": [1, 1, 1]}], "environment": {"glow": 1}}]})"),
                                                 "x.json")
                     .has_value());
    CHECK_FALSE(rv::scene::parseSceneDescription(nlohmann::json::parse(R"({"schemaVersion": 1, "name": "x",
        "areas": [{"id": "a", "boxes": [{"min": [0, 0, 0], "max": [1, 1, 1]}], "environment": {"exposure": 0}}]})"),
                                                 "x.json")
                     .has_value());
    CHECK_FALSE(rv::scene::parseSceneDescription(
                    nlohmann::json::parse(R"({"schemaVersion": 1, "name": "x", "environment": {"fillIntensity": -1}})"),
                    "x.json")
                    .has_value());

    // A scene without areas reads as before: no area, lights without one.
    const auto plain = rv::scene::parseSceneDescription(
        nlohmann::json::parse(R"({"schemaVersion": 1, "name": "plain", "lights": [{"type": "point", "position": [0, 2, 0]}]})"),
        "plain.json");
    REQUIRE(plain.has_value());
    CHECK(plain->areas.empty());
    CHECK(plain->lights[0].area.empty());
}
