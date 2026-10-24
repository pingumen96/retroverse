// The areas of a scene (rv/scene/scene_areas.hpp, I4 step 3, ADR-0013 §9; I6 step 1, ADR-0014 §4 as amended): the area
// that holds the eye (the first holding it, else the last one held), the lights a view packs there (the area's own,
// its key light first, the nearest to the eye in the capacity its neighbours' share leaves; the neighbours' seen
// through a portal by importance, never the key light, clipped to their portal; none wholly behind the eye), a full
// floor's views (a hall facing a doorway packs that room's lights, the room the hall's), the stability of the
// selection (a still eye, a slight turn, a 2 cm strafe, a slow turn all around and a walk down a hall toggle nothing),
// the fades (no light comes or goes in a frame, alike on every run, at once at a cut), the environment eased between
// areas, and the areas read from a scene file.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>
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

TEST_CASE("scene/areas: a neighbour's lights: a share seen through the portal, by importance, never the key light",
          "[scene][areas]") {
    // A room north of a hall, its doorway's passage at x in [-1, 1], z in [-0.2, 0].
    SceneArea room = area("room", {-5.0f, 0.0f, -10.0f}, {5.0f, 3.2f, -0.2f});
    room.boxes.push_back(rv::core::Aabb::fromMinMax({-1.0f, 0.0f, -0.2f}, {1.0f, 2.3f, 0.0f}));
    const std::vector<SceneArea> areas{room, area("hall", {-20.0f, 0.0f, 0.0f}, {20.0f, 3.2f, 3.0f})};
    std::vector<Light> lights;
    // The room's key spot by the doorway, six lights at 1.5 m to 6.5 m from the portal, a strong one far in, a weak
    // one near, and one out of reach (beyond kNeighbourReachM plus its range).
    Light key = pointLight("key", "room", {0.0f, 2.9f, -1.0f});
    key.type = LightType::Spot;
    key.castsShadows = true;
    lights.push_back(key);
    for (int i = 1; i <= 6; ++i) {
        lights.push_back(pointLight("r" + std::to_string(i), "room", {0.0f, 1.5f, -static_cast<float>(i) - 0.5f}));
    }
    lights.push_back(pointLight("far", "room", {0.0f, 1.5f, -9.0f}));
    lights.back().intensity = 100.0f; // 100 / (1 + 11^2): more than r2's 1 / (1 + 4.5^2)
    lights.push_back(pointLight("weak", "room", {0.0f, 1.5f, -2.0f}));
    lights.back().intensity = 0.01f;
    lights.push_back(pointLight("hall", "hall", {0.0f, 2.9f, 1.5f}));
    lights.push_back(pointLight("beyond", "room", {4.5f, 1.5f, -9.9f}));
    lights.back().range = 0.5f; // 10.8 m from the portal
    const std::uint32_t hallLight = 9;
    const rv::core::Aabb portal = rv::core::Aabb::fromMinMax({-1.0f, 0.0f, 0.0f}, {1.0f, 2.3f, 0.0f});

    AreaLighting lighting(areas, lights, {0.0f, 1.65f, 2.0f}, 16);
    REQUIRE(lighting.area() == 1u);
    // The hall's own light, then the room's: the hall's one light leaves 15 slots, more than the room's 9 lights
    // that may be seen through the portal (never "beyond").
    CHECK(lighting.ownCount() == 1);
    CHECK(selected(lighting) == std::vector<std::uint32_t>{hallLight, 0, 1, 2, 3, 4, 5, 6, 7, 8});
    // Each clipped to the portal (the opening, up to the doorway's head): the room's lights, and the hall's own, since
    // this hall has a single portal (a hall of several portals lights them all unclipped).
    REQUIRE(lighting.portals().size() == 10);
    for (const rv::core::Aabb& clip : lighting.portals()) {
        CHECK(clip == portal);
    }
    CHECK(std::all_of(lighting.weights().begin(), lighting.weights().end(), [](float w) { return w == 1.0f; }));
    // The room's key light is no key light from the hall: packLights' keyCandidates (ownCount()) leave it out, and
    // keyLight() is the eye's area's own (none here).
    CHECK_FALSE(lighting.keyLight().has_value());

    // A tight capacity: the neighbours take the slots the hall's one light leaves, the most important first: far by
    // its intensity, then the key (a path of 3.4 m through the portal), then r1 (3.5 m).
    AreaLighting tight(areas, lights, {0.0f, 1.65f, 2.0f}, 4);
    CHECK(tight.ownCount() == 1);
    CHECK(selected(tight) == std::vector<std::uint32_t>{hallLight, 0, 1, 7});
    AreaLighting full(areas, lights, {0.0f, 1.65f, 2.0f}, 1);
    CHECK(selected(full) == std::vector<std::uint32_t>{hallLight});

    // From the room, its own lights first (its key light among them, the area's key), the hall's light after them,
    // clipped to the same portal, as the room's own lights are (a room has one portal).
    CHECK(lighting.update(areas, lights, {0.0f, 1.65f, -5.0f}));
    CHECK(lighting.area() == 0u);
    CHECK(lighting.ownCount() == 10);
    CHECK(selected(lighting) == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 10, hallLight});
    CHECK(lighting.keyLight() == 0u);
    for (const rv::core::Aabb& clip : lighting.portals()) {
        CHECK(clip == portal);
    }
}

namespace {

/// A full floor (I6 step 1's review): a hall 3 m wide along x with a light every 6 m and twelve rooms in each row, each
/// 9.3 m apart with a 1.6 m doorway onto the hall and 15 lights as a generated room has them (a key spot, a pendant,
/// four washers, three lamps, a sign spot, a CRT glow, a sign glow, two case spots, a shelf spot), and a fill spot in
/// the hall before each doorway.
struct FullFloor {
    std::vector<SceneArea> areas;
    std::vector<Light> lights;
    /// The doorways' middles along x.
    std::vector<float> doorways;
};

FullFloor fullFloor() {
    FullFloor floor;
    constexpr int kPerRow = 12;
    const float length = 3.0f + 9.3f * kPerRow;
    for (int row = 0; row < 2; ++row) {
        const float s = row == 0 ? -1.0f : 1.0f; // north: -z
        for (int i = 0; i < kPerRow; ++i) {
            const float c = 3.0f + 4.65f + 9.3f * static_cast<float>(i);
            const std::string id = "room:" + std::to_string(row) + "-" + std::to_string(i);
            const auto at = [&](float x, float y, float depth) { return glm::vec3(c + x, y, s * (1.65f + depth)); };
            SceneArea room;
            room.id = id;
            const glm::vec3 a = at(-4.5f, 0.0f, 0.0f);
            const glm::vec3 b = at(4.5f, 3.2f, 10.0f);
            room.boxes.push_back(rv::core::Aabb::fromMinMax(glm::min(a, b), glm::max(a, b)));
            const glm::vec3 p = at(-0.8f, 0.0f, -0.15f);
            const glm::vec3 q = at(0.8f, 2.3f, 0.0f);
            room.boxes.push_back(rv::core::Aabb::fromMinMax(glm::min(p, q), glm::max(p, q)));
            floor.areas.push_back(room);
            if (row == 0) {
                floor.doorways.push_back(c);
            }
            const auto add = [&](std::string name, LightType type, glm::vec3 position, float intensity, float range) {
                Light light = pointLight(id + "/" + name, id, position);
                light.type = type;
                light.intensity = intensity;
                light.range = range;
                light.direction = glm::vec3(0.0f, -1.0f, 0.0f);
                light.innerConeDeg = 20.0f;
                light.outerConeDeg = 35.0f;
                floor.lights.push_back(light);
            };
            add("key", LightType::Spot, at(0.0f, 2.9f, 8.5f), 70.0f, 12.0f);
            floor.lights.back().castsShadows = true;
            add("pendant", LightType::Point, at(0.0f, 2.6f, 5.0f), 16.0f, 10.0f);
            add("washer-1", LightType::Spot, at(-3.8f, 2.2f, 3.0f), 12.0f, 5.0f);
            add("washer-2", LightType::Spot, at(3.8f, 2.2f, 3.0f), 12.0f, 5.0f);
            add("washer-3", LightType::Spot, at(-3.8f, 2.2f, 7.5f), 12.0f, 5.0f);
            add("washer-4", LightType::Spot, at(3.8f, 2.2f, 7.5f), 12.0f, 5.0f);
            add("lamp-1", LightType::Point, at(-3.6f, 1.5f, 1.0f), 4.0f, 4.0f);
            add("lamp-2", LightType::Point, at(3.6f, 1.5f, 1.0f), 4.0f, 4.0f);
            add("lamp-3", LightType::Point, at(-1.5f, 1.5f, 9.3f), 4.0f, 4.0f);
            add("sign", LightType::Spot, at(0.0f, 2.8f, 9.0f), 8.0f, 4.0f);
            add("crt-glow", LightType::Spot, at(-1.0f, 0.9f, 8.8f), 2.5f, 3.5f);
            add("sign-glow", LightType::Point, at(0.0f, 2.5f, 9.6f), 1.5f, 2.5f);
            add("case-1", LightType::Spot, at(-1.5f, 1.95f, 4.5f), 5.0f, 3.5f);
            add("case-2", LightType::Spot, at(1.5f, 1.95f, 4.5f), 5.0f, 3.5f);
            add("shelf", LightType::Spot, at(3.0f, 2.6f, 8.7f), 8.0f, 3.5f);
            add("fill", LightType::Spot, glm::vec3(c, 2.0f, -s * 0.6f), 3.5f, 6.0f);
            floor.lights.back().area = "hall";
            floor.lights.back().name = "hall/fill:" + id;
        }
    }
    for (float x = 3.0f; x < length; x += 6.0f) {
        Light light = pointLight("hall/light", "hall", {x, 2.6f, 0.0f});
        light.intensity = 12.0f;
        light.range = 9.0f;
        floor.lights.push_back(light);
    }
    floor.areas.push_back(area("hall", {0.0f, 0.0f, -1.5f}, {length, 3.2f, 1.5f}));
    return floor;
}

/// The unit view direction of a yaw and pitch (degrees; rv::scene::Camera's convention: yaw 0 looks along -z).
glm::vec3 forwardOf(float yawDeg, float pitchDeg) {
    const float yaw = glm::radians(yawDeg);
    const float pitch = glm::radians(pitchDeg);
    return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch)};
}

/// The lights of `lighting`'s selection whose names start with `prefix`.
std::size_t countOf(const AreaLighting& lighting, const std::vector<Light>& lights, const std::string& prefix) {
    return static_cast<std::size_t>(
        std::count_if(lighting.selection().begin(), lighting.selection().end(),
                      [&](std::uint32_t index) { return lights[index].name.rfind(prefix, 0) == 0; }));
}

/// Plays `poses` (eye, forward) at 60 frames a second and counts, per light, how often it was chosen or left out
/// again less than kLightDwellSeconds after its last change (a toggle), and in `changes` (when given) how often each
/// light was chosen or left out in all.
std::size_t toggles(AreaLighting& lighting, const FullFloor& floor,
                    const std::vector<std::pair<glm::vec3, glm::vec3>>& poses,
                    std::vector<std::size_t>* changes = nullptr) {
    const auto window = static_cast<std::size_t>(rv::scene::kLightDwellSeconds * 60.0f);
    if (changes != nullptr) {
        changes->assign(floor.lights.size(), 0);
    }
    std::vector<std::uint8_t> chosen(floor.lights.size(), 0);
    std::vector<std::size_t> lastChange(floor.lights.size(), 0);
    std::vector<bool> changed(floor.lights.size(), false);
    std::size_t count = 0;
    for (std::size_t frame = 0; frame < poses.size(); ++frame) {
        (void)lighting.update(floor.areas, floor.lights, poses[frame].first, poses[frame].second, 1.0f / 60.0f);
        std::vector<std::uint8_t> now(floor.lights.size(), 0);
        for (const std::uint32_t index : lighting.chosen()) {
            now[index] = 1;
        }
        for (std::size_t index = 0; index < now.size(); ++index) {
            if (frame > 0 && now[index] != chosen[index]) {
                if (changes != nullptr) {
                    ++(*changes)[index];
                }
                if (changed[index] && frame - lastChange[index] < window) {
                    ++count;
                    UNSCOPED_INFO("toggle of " << floor.lights[index].name << " at frame " << frame);
                }
                changed[index] = true;
                lastChange[index] = frame;
            }
        }
        chosen = now;
    }
    return count;
}

} // namespace

TEST_CASE("scene/areas: a full floor: the hall facing a doorway packs that room's lights, the room the hall's",
          "[scene][areas]") {
    const FullFloor floor = fullFloor();
    for (const std::size_t room : {std::size_t{0}, std::size_t{5}, std::size_t{11}}) {
        const float x = floor.doorways[room];
        const std::string id = "room:0-" + std::to_string(room) + "/";
        INFO(id);
        // From the hall's far side, facing the north doorway.
        AreaLighting lighting(floor.areas, floor.lights, {x, 1.65f, 1.1f}, 16);
        (void)lighting.update(floor.areas, floor.lights, {x, 1.65f, 1.1f}, forwardOf(0.0f, -4.0f));
        CHECK(floor.areas[*lighting.area()].id == "hall");
        CHECK(lighting.selection().size() <= 16);
        // The hall packs at most its share; the faced room's lights take most of the neighbours' (its key, unshadowed,
        // among them), the faced doorway's fill among the hall's own.
        CHECK(lighting.ownCount() == 16 - rv::scene::kNeighbourLightReserve);
        CHECK(countOf(lighting, floor.lights, id) >= 4);
        CHECK(countOf(lighting, floor.lights, id + "key") == 1);
        CHECK(countOf(lighting, floor.lights, "hall/fill:room:0-" + std::to_string(room)) == 1);
        CHECK_FALSE(lighting.keyLight().has_value());

        // From 4.5 m inside, facing out: the room's own (its key light first among equals), then up to the share of
        // the hall's, its doorway's fill among them.
        (void)lighting.update(floor.areas, floor.lights, {x, 1.65f, -6.15f}, forwardOf(180.0f, -4.0f));
        CHECK(floor.areas[*lighting.area()].id == "room:0-" + std::to_string(room));
        CHECK(lighting.selection().size() <= 16);
        CHECK(lighting.ownCount() >= 16 - rv::scene::kNeighbourLightReserve);
        CHECK(lighting.selection().size() - lighting.ownCount() >= 3);
        CHECK(countOf(lighting, floor.lights, "hall/fill:room:0-" + std::to_string(room)) == 1);
        REQUIRE(lighting.keyLight().has_value());
        CHECK(floor.lights[*lighting.keyLight()].name == id + "key");
    }
}

TEST_CASE("scene/areas: a still eye, a slow turn, a small strafe and a walk toggle no light", "[scene][areas]") {
    const FullFloor floor = fullFloor();
    const float x = floor.doorways[4];
    std::vector<std::pair<glm::vec3, glm::vec3>> poses;
    // Still, in the hall facing a doorway, then in a room facing out (the review's spot: yaw 195.5 to 195.9 and back,
    // pitch -20).
    const glm::vec3 hallEye(x, 1.65f, 1.1f);
    const glm::vec3 roomEye(x + 0.62f, 1.65f, -8.53f);
    for (int frame = 0; frame < 60; ++frame) {
        poses.emplace_back(hallEye, forwardOf(0.0f, -4.0f));
    }
    AreaLighting still(floor.areas, floor.lights, hallEye, 16);
    CHECK(toggles(still, floor, poses) == 0);
    poses.clear();
    for (int frame = 0; frame < 120; ++frame) {
        poses.emplace_back(roomEye, forwardOf(frame % 2 == 0 ? 195.5f : 195.9f, -20.0f));
    }
    AreaLighting turning(floor.areas, floor.lights, roomEye, 16);
    (void)turning.update(floor.areas, floor.lights, roomEye, forwardOf(195.5f, -20.0f));
    const std::vector<std::uint32_t> first = turning.chosen();
    CHECK(toggles(turning, floor, poses) == 0);
    CHECK(turning.chosen() == first);
    // A strafe of 2 cm to and fro, in the room and in the hall.
    poses.clear();
    for (int frame = 0; frame < 120; ++frame) {
        const float dx = frame % 2 == 0 ? 0.0f : 0.02f;
        poses.emplace_back(roomEye + glm::vec3(dx, 0.0f, 0.0f), forwardOf(195.7f, -20.0f));
    }
    CHECK(toggles(turning, floor, poses) == 0);
    CHECK(turning.chosen() == first);
    poses.clear();
    for (int frame = 0; frame < 120; ++frame) {
        const float dx = frame % 2 == 0 ? 0.0f : 0.02f;
        poses.emplace_back(hallEye + glm::vec3(dx, 0.0f, 0.0f), forwardOf(0.0f, -4.0f));
    }
    CHECK(toggles(still, floor, poses) == 0);

    // A slow turn all around (0.2 degrees a frame, 30 seconds), in the room and in the hall: a light chosen stays for
    // half a second at least.
    for (const glm::vec3& eye : {roomEye, hallEye}) {
        poses.clear();
        for (int frame = 0; frame < 1800; ++frame) {
            poses.emplace_back(eye, forwardOf(0.2f * static_cast<float>(frame), -20.0f));
        }
        AreaLighting sweep(floor.areas, floor.lights, eye, 16);
        CHECK(toggles(sweep, floor, poses) == 0);
    }
    // A walk down the whole hall at 2.5 m/s, looking ahead, then back looking ahead: the lights come as they near and
    // go once passed (the hall's own among its doorway fills now and then twice), never back within kLightDwellSeconds:
    // at most three times each way.
    poses.clear();
    const float length = floor.areas.back().boxes[0].max.x;
    for (float walked = 1.0f; walked < length - 1.0f; walked += 2.5f / 60.0f) {
        poses.emplace_back(glm::vec3(walked, 1.65f, 0.0f), forwardOf(90.0f, 0.0f));
    }
    for (float walked = length - 1.0f; walked > 1.0f; walked -= 2.5f / 60.0f) {
        poses.emplace_back(glm::vec3(walked, 1.65f, 0.0f), forwardOf(270.0f, 0.0f));
    }
    AreaLighting walk(floor.areas, floor.lights, poses.front().first, 16);
    std::vector<std::size_t> changes;
    CHECK(toggles(walk, floor, poses, &changes) == 0);
    for (std::size_t index = 0; index < changes.size(); ++index) {
        INFO(floor.lights[index].name);
        CHECK(changes[index] <= 6);
    }
}

TEST_CASE("scene/areas: a light fades in and out over kLightFadeSeconds, at once at a cut", "[scene][areas]") {
    const FullFloor floor = fullFloor();
    const float x = floor.doorways[3];
    // Walk from the hall through the doorway into the room at 2.5 m/s, 60 frames a second.
    std::vector<glm::vec3> eyes;
    for (float z = 1.1f; z > -8.0f; z -= 2.5f / 60.0f) {
        eyes.emplace_back(x, 1.65f, z);
    }
    // Then a second's stop.
    eyes.insert(eyes.end(), 60, eyes.back());
    const auto play = [&](std::vector<std::vector<float>>& weights) {
        AreaLighting lighting(floor.areas, floor.lights, eyes.front(), 16);
        std::vector<float> previous(floor.lights.size(), -1.0f);
        bool entered = false;
        for (std::size_t frame = 0; frame < eyes.size(); ++frame) {
            (void)lighting.update(floor.areas, floor.lights, eyes[frame], forwardOf(0.0f, -4.0f), 1.0f / 60.0f);
            REQUIRE(lighting.selection().size() <= 16);
            REQUIRE(lighting.weights().size() == lighting.selection().size());
            std::vector<float> now(floor.lights.size(), 0.0f);
            for (std::size_t i = 0; i < lighting.selection().size(); ++i) {
                const float weight = lighting.weights()[i];
                CHECK(weight >= 0.0f);
                CHECK(weight <= 1.0f);
                now[lighting.selection()[i]] = weight;
            }
            for (std::size_t index = 0; index < now.size(); ++index) {
                // No light comes or goes in one frame: a fade of 0.3 s moves the smoothstep by at most 1.5 x 1/18.
                if (frame > 0) {
                    INFO(floor.lights[index].name << " at frame " << frame);
                    CHECK(std::abs(now[index] - std::max(0.0f, previous[index])) <= 1.5f / 18.0f + 1e-4f);
                }
            }
            entered = entered || floor.areas[*lighting.area()].id == "room:0-3";
            previous = now;
            weights.push_back(std::move(now));
        }
        CHECK(entered);
        // Long after the doorway the fades are done: every light chosen is packed whole.
        CHECK_FALSE(lighting.fading());
        std::vector<std::uint32_t> packed(lighting.selection().begin(), lighting.selection().end());
        std::sort(packed.begin(), packed.end());
        CHECK(packed == lighting.chosen());
    };
    std::vector<std::vector<float>> first;
    std::vector<std::vector<float>> second;
    play(first);
    play(second);
    CHECK(first == second); // the same steps, the same fades
    // Some light faded in and some out on the way (the room's from the hall's share to its own, the hall's back).
    std::size_t fadedIn = 0;
    std::size_t fadedOut = 0;
    for (std::size_t index = 0; index < floor.lights.size(); ++index) {
        bool partial = false;
        for (const std::vector<float>& frame : first) {
            partial = partial || (frame[index] > 0.0f && frame[index] < 1.0f);
        }
        if (partial) {
            (first.back()[index] == 1.0f ? fadedIn : fadedOut) += 1;
        }
    }
    CHECK(fadedIn > 0);
    CHECK(fadedOut > 0);

    // A cut (a teleport) and reselect() are at once: every light chosen whole, nothing fading.
    AreaLighting lighting(floor.areas, floor.lights, eyes.front(), 16);
    (void)lighting.update(floor.areas, floor.lights, {floor.doorways[9], 1.65f, -6.0f}, forwardOf(180.0f, 0.0f),
                          1.0f / 60.0f, true);
    CHECK_FALSE(lighting.fading());
    CHECK(std::all_of(lighting.weights().begin(), lighting.weights().end(), [](float w) { return w == 1.0f; }));
    // A step of no time moves no fade; a bad one neither.
    (void)lighting.update(floor.areas, floor.lights, {floor.doorways[9], 1.65f, 1.0f}, forwardOf(0.0f, 0.0f), 0.0f);
    const std::vector<float> held(lighting.weights().begin(), lighting.weights().end());
    (void)lighting.update(floor.areas, floor.lights, {floor.doorways[9], 1.65f, 1.0f}, forwardOf(0.0f, 0.0f),
                          std::numeric_limits<float>::quiet_NaN());
    CHECK(std::vector<float>(lighting.weights().begin(), lighting.weights().end()) == held);
    CHECK(lighting.fading());
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
