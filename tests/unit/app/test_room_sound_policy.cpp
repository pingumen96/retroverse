// The rooms' sound as the application decides it (rv/app/room_sound_policy.hpp, I12 step 7, ADR-0021 §5): a scene's
// "sound" read and refused out of its ranges; the emitters of each area (the hum and hiss over the hero CRT, the buzz
// at its sign, the murmur and the clock at the room's centre), heard only in the eye's area, lower under a clip, the
// hiss only while the hero shows a picture; the floor under the walker; the steps' cadence; the interaction's edges.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include <nlohmann/json.hpp>

#include <rv/app/room_sound_policy.hpp>
#include <rv/audio/spatial.hpp>
#include <rv/scene/scene_description.hpp>

using Catch::Approx;
using rv::app::FloorSurface;
using rv::app::WorldState;
using rv::audio::SoundId;

namespace {

/// Two rooms side by side on x (each 4 m), the first with a hero CRT and sign, the second the arcade's murmur; a hall
/// without a tone.
rv::scene::SceneDescription twoRooms() {
    const nlohmann::json document = nlohmann::json::parse(R"({
        "schemaVersion": 1, "name": "tone",
        "areas": [
            {"id": "room:a", "boxes": [{"min": [0, 0, 0], "max": [4, 3, 4]}],
             "environment": {"sound": {"hum": 1, "hiss": 1, "buzz": 0.5}}},
            {"id": "room:b", "boxes": [{"min": [4, 0, 0], "max": [8, 3, 4]}],
             "environment": {"sound": {"hum": 0, "hiss": 0, "buzz": 0, "murmur": 1, "clock": 2}}},
            {"id": "hall", "boxes": [{"min": [0, 0, 4], "max": [8, 3, 6]}]}
        ],
        "objects": [
            {"id": "room:a/fx:hero.crt", "primitive": "box", "position": [1, 0.6, 0.5]},
            {"id": "room:a/fx:hero.sign", "primitive": "box", "position": [2, 2.2, 0.1]},
            {"id": "room:b/fx:hero.crt", "primitive": "box", "position": [5, 0.6, 0.5]},
            {"id": "hall/fx:hero.crt", "primitive": "box", "position": [3, 0.6, 5]}
        ]
    })");
    auto scene = rv::scene::parseSceneDescription(document, "tone.json");
    REQUIRE(scene.has_value());
    return *scene;
}

} // namespace

TEST_CASE("app/room sound: a scene's tone read from its file, refused out of its ranges", "[app][room_sound]") {
    const rv::scene::SceneDescription scene = twoRooms();
    REQUIRE(scene.areas[0].environment.sound.has_value());
    CHECK(scene.areas[0].environment.sound->buzz == 0.5f);
    CHECK(scene.areas[0].environment.sound->murmur == 0.0f); // the defaults for what it leaves out
    CHECK_FALSE(scene.areas[2].environment.sound.has_value());
    CHECK_FALSE(scene.environment.sound.has_value());
    CHECK(rv::app::roomSoundOf(scene, 1)->clock == 2.0f);
    CHECK_FALSE(rv::app::roomSoundOf(scene, 2).has_value());
    for (const char* bad : {R"({"hum": 3})", R"({"buzz": -0.1})", R"({"fan": 1})", R"({"hum": "loud"})"}) {
        INFO(bad);
        nlohmann::json document{
            {"schemaVersion", 1}, {"name", "bad"}, {"environment", {{"sound", nlohmann::json::parse(bad)}}}};
        CHECK_FALSE(rv::scene::parseSceneDescription(document, "bad.json").has_value());
    }
}

TEST_CASE("app/room sound: each room's emitters, heard in the eye's room alone", "[app][room_sound]") {
    const rv::scene::SceneDescription scene = twoRooms();
    const std::vector<rv::app::RoomToneEmitter> emitters = rv::app::roomToneEmitters(scene);
    // room:a's hum, hiss and buzz; room:b's CRT silent (hum 0), its murmur and clock at its centre; the hall none.
    REQUIRE(emitters.size() == 5);
    CHECK(emitters[0].sound == SoundId::CrtHum);
    CHECK(emitters[0].area == 0u);
    CHECK(emitters[0].position.y == Approx(0.85f));
    CHECK(emitters[0].gain == Approx(rv::app::kHumLevel));
    CHECK(emitters[1].sound == SoundId::PhosphorHiss);
    CHECK(emitters[1].whileShowing);
    CHECK(emitters[2].sound == SoundId::NeonBuzz);
    CHECK(emitters[2].gain == Approx(0.5 * rv::app::kBuzzLevel));
    CHECK(emitters[3].sound == SoundId::RoomMurmur);
    CHECK(emitters[3].area == 1u);
    CHECK(emitters[3].position.x == Approx(6.0f));
    CHECK(emitters[3].position.y == Approx(1.6f));
    CHECK(emitters[3].at == "room:b/centre");
    CHECK(emitters[4].sound == SoundId::ClockTick);
    CHECK(emitters[4].gain == Approx(2.0 * rv::app::kClockLevel));

    const glm::vec3 eye(1.0f, 1.6f, 2.0f);
    const glm::vec3 forward(0.0f, 0.0f, -1.0f);
    // The hum 1.6 m ahead: about full gain, centred.
    const rv::app::RoomToneMix hum = rv::app::roomToneMix(emitters[0], eye, forward, 0u, false, false);
    CHECK(hum.gain == Approx(rv::app::kHumLevel)); // 1.68 m: within the full gain's reach
    CHECK(hum.pan == Approx(0.0).margin(1e-6));
    // Under a clip, 6 dB lower.
    CHECK(rv::app::roomToneMix(emitters[0], eye, forward, 0u, false, true).gain ==
          Approx(hum.gain * rv::audio::gainFromDecibels(rv::app::kRoomToneUnderClipDecibels)));
    // The hiss only while a picture shows.
    CHECK(rv::app::roomToneMix(emitters[1], eye, forward, 0u, false, false).gain == 0.0);
    CHECK(rv::app::roomToneMix(emitters[1], eye, forward, 0u, true, false).gain > 0.0);
    // The other room's murmur: silent from room:a, heard from room:b, to the right when facing -z from its west side.
    CHECK(rv::app::roomToneMix(emitters[3], eye, forward, 0u, false, false).gain == 0.0);
    const rv::app::RoomToneMix murmur =
        rv::app::roomToneMix(emitters[3], glm::vec3(4.5f, 1.6f, 2.0f), forward, 1u, false, false);
    CHECK(murmur.gain > 0.0);
    CHECK(murmur.pan > 0.0);
    // Across the room, never below the floor of its reverberant field.
    CHECK(rv::app::roomToneDistanceGain(1.0) == 1.0);
    CHECK(rv::app::roomToneDistanceGain(4.0) == Approx(0.5));
    CHECK(rv::app::roomToneDistanceGain(30.0) == rv::app::kRoomToneFloor);
    CHECK(rv::app::roomToneMix(emitters[3], glm::vec3(6.0f, 1.6f, 40.0f), forward, 1u, false, false).gain ==
          Approx(rv::app::kMurmurLevel * rv::app::kRoomToneFloor));
}

TEST_CASE("app/room sound: the floor under the walker, the steps' cadence, the interaction's edges",
          "[app][room_sound]") {
    nlohmann::json document = nlohmann::json::parse(R"({
        "schemaVersion": 1, "name": "floor",
        "objects": [
            {"id": "room:a/dressing:runner", "prefab": "rug", "params": {"width": 1, "depth": 4, "thickness": 0.01},
             "position": [0, 0, 0]},
            {"id": "room:a/fx:hero.rug", "prefab": "rug", "params": {"width": 2, "depth": 2, "thickness": 0.01},
             "position": [0, 0, 1]},
            {"id": "room:a/fx:w01", "primitive": "box", "position": [3, 0, 0]}
        ]
    })");
    auto scene = rv::scene::parseSceneDescription(document, "floor.json");
    REQUIRE(scene.has_value());
    // Their local bounds as the instance would give them (centred footprints).
    const std::vector<rv::core::Aabb> bounds{rv::core::Aabb::fromMinMax({-0.5f, 0.0f, -2.0f}, {0.5f, 0.01f, 2.0f}),
                                             rv::core::Aabb::fromMinMax({-1.0f, 0.0f, -1.0f}, {1.0f, 0.01f, 1.0f}),
                                             rv::core::Aabb::fromMinMax({-0.5f, 0.0f, -0.5f}, {0.5f, 1.0f, 0.5f})};
    const std::vector<rv::app::FloorPatch> patches = rv::app::floorPatches(*scene, bounds);
    REQUIRE(patches.size() == 2);
    CHECK(patches[0].surface == FloorSurface::Runner);
    CHECK(patches[1].surface == FloorSurface::Rug);
    CHECK(rv::app::surfaceUnder({0.0f, -1.5f}, patches) == FloorSurface::Runner);
    CHECK(rv::app::surfaceUnder({0.0f, 1.2f}, patches) == FloorSurface::Rug); // the rug over the runner
    CHECK(rv::app::surfaceUnder({0.9f, 1.5f}, patches) == FloorSurface::Rug);
    CHECK(rv::app::surfaceUnder({3.0f, 0.0f}, patches) == FloorSurface::Parquet);
    CHECK(rv::app::footstepSound(FloorSurface::Rug) == SoundId::FootstepRug);
    CHECK(rv::app::footstepSound(FloorSurface::Parquet) == SoundId::FootstepParquet);

    // Walking 1.4 m/s for 2 s at 60 Hz: the first step half a stride on, then one each 0.72 m, feet alternating.
    rv::app::FootstepCadence cadence;
    const double dt = 1.0 / 60.0;
    std::vector<bool> feet;
    for (int frame = 0; frame <= 120; ++frame) {
        const glm::vec2 at(0.0f, static_cast<float>(1.4 * dt * frame));
        if (const auto step = cadence.advance(at, dt, true)) {
            feet.push_back(step->left);
        }
    }
    // 2.8 m walked: steps at 0.36, 1.08, 1.80, 2.52 m.
    REQUIRE(feet.size() == 4);
    CHECK(feet[0] != feet[1]);
    CHECK(feet[1] != feet[2]);
    CHECK(cadence.steps() == 4);
    // Sprinting (3 m/s): longer strides.
    rv::app::FootstepCadence sprint;
    std::size_t sprintSteps = 0;
    for (int frame = 0; frame <= 120; ++frame) {
        sprintSteps += sprint.advance({0.0f, static_cast<float>(3.0 * dt * frame)}, dt, true) ? 1 : 0;
    }
    CHECK(sprintSteps == 6); // 6 m: 0.36, then every 0.95 m
    // Held (a menu, the interaction's ease) or teleported: no step, and the cadence starts again.
    rv::app::FootstepCadence held;
    CHECK_FALSE(held.advance({0.0f, 0.0f}, dt, true));
    CHECK_FALSE(held.advance({0.0f, 0.3f}, dt, false));
    CHECK_FALSE(held.advance({0.0f, 5.0f}, dt, true));
    CHECK_FALSE(held.advance({0.0f, 9.0f}, dt, true)); // 4 m in a frame: a teleport
    CHECK(held.steps() == 0);

    CHECK(rv::app::interactionSound(WorldState::FreeRoam, WorldState::Focus) == SoundId::ItemClack);
    CHECK(rv::app::interactionSound(WorldState::Focus, WorldState::FreeRoam) == SoundId::ItemClack);
    CHECK(rv::app::interactionSound(WorldState::Focus, WorldState::Inspect) == SoundId::CaseOpen);
    CHECK(rv::app::interactionSound(WorldState::Inspect, WorldState::Focus) == SoundId::CaseOpen);
    CHECK(rv::app::interactionSound(WorldState::AnimatingToConsole, WorldState::Launching) == SoundId::InsertionClick);
    CHECK_FALSE(rv::app::interactionSound(WorldState::Launching, WorldState::Returning).has_value());
    CHECK_FALSE(rv::app::interactionSound(WorldState::Focus, WorldState::Focus).has_value());
}
