// The editor's furniture in the app (I4b step 4; ADR-0015 §4): the building fixture's arcade room (its side-wall
// modules are cabinets; room:snes has none), retroverse.exe --start-room area:arcade, lockstep (--fixed-dt), hidden,
// driven by a script the test writes from the plan as planned (where the walker stands and what the reticle rests on
// depend on it): from a place in the room that sees a poster on a wall and a side-wall module where no game is, the
// directory's "Modifica stanza"; the poster picked up, slid along its wall with the D-pad and dropped (its static part
// rebuilt alone); the view turned onto the module, the module picked up, nudged on the grid to a place it may drop
// (outlined warm white), then further to one a rule refuses, dropped there (refused: the bar names the rule, the
// outline red), nudged back and dropped (the room's static part rebuilt alone: its scene objects, dressing,
// wall-washers and colliders), the menu, Salva; then the walker walks into the module where it stands now and stops
// against it (its colliders moved with it). The stored plan holds both where they were put, pinned by the editor; a
// restart writes nothing. Screenshots: holding the module where it may drop, the refused drop with the bar's rule, the
// room after the save (build/smoke/world_edit_furniture/*.png).
//
// [.real-library][edit][furniture]: the same session's measure on the real library's computer floor (a fixture moved
// and saved), through scripts/real-library.ps1 -Measure (see test_world_edit.cpp's real-library case).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/command_line.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_edit_room.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/math.hpp>
#include <rv/world/building.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/validation.hpp>
#include <rv/world/world_config.hpp>
#include <rv/world/world_edit.hpp>
#include <rv/world/world_store.hpp>

#include "building_support.hpp"
#include "smoke_support.hpp"
#include "support/scoped_environment.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// The building fixture's arcade room: side-wall modules (cabinets) and posters on its side walls.
constexpr std::string_view kRoom = "area:arcade";
/// The walker's eye above the feet, the mouse's look per pixel, the capsule's radius (scene::FirstPersonSettings').
constexpr float kEyeHeight = 1.65f;
constexpr float kRadiansPerPixel = 0.0022f;
constexpr float kRadius = 0.30f;
const rv::world::PinProvenance kProvenance{rv::world::PinOrigin::Editor, "2026-10-28T12:00:00.000Z"};

fs::path buildAssets() {
    return buildConfig().devAssetRoot ? sourceDir() / "assets" : retroverseExe().parent_path() / "assets";
}

/// The yaw and pitch that look along `direction` (scene::Camera's conventions: yaw 0 towards -Z, positive to the right
/// and up).
glm::vec2 lookAlong(const glm::vec3& direction) {
    return {std::atan2(direction.x, -direction.z), std::asin(std::clamp(direction.y, -1.0f, 1.0f))};
}

/// `to - from` in mouse pixels, the yaw's difference taken the short way round.
glm::vec2 turnPixels(glm::vec2 from, glm::vec2 to) {
    glm::vec2 turn = to - from;
    while (turn.x > std::numbers::pi_v<float>) {
        turn.x -= 2.0f * std::numbers::pi_v<float>;
    }
    while (turn.x <= -std::numbers::pi_v<float>) {
        turn.x += 2.0f * std::numbers::pi_v<float>;
    }
    return turn / kRadiansPerPixel;
}

/// What an edit of the floor of `room` reads (app::prepareEditData()'s: the configuration, the building, every room of
/// the floor), from the data directory `data`.
rv::world::EditData editDataOf(const fs::path& data, const rv::world::WorldPlan& plan, const rv::world::RoomId& room) {
    auto config = rv::world::loadWorldConfig(buildAssets(), data / "config");
    REQUIRE(config.has_value());
    auto building = rv::world::loadBuildingData(buildAssets());
    REQUIRE(building.has_value());
    rv::world::EditData edit;
    edit.config = config->config;
    edit.building = std::move(*building);
    std::string floor;
    for (const rv::world::Room& candidate : plan.rooms) {
        if (candidate.id == room) {
            floor = candidate.floor;
        }
    }
    for (const rv::world::Room& candidate : plan.rooms) {
        if (candidate.floor == floor) {
            auto loaded = rv::world::loadRoomData(buildAssets(), candidate);
            REQUIRE(loaded.has_value());
            edit.rooms.emplace(candidate.id, std::make_shared<const rv::world::WorldData>(std::move(*loaded)));
        }
    }
    return edit;
}

/// The view's forward on the floor snapped to the room's axis nearest it (the editor's nudges: EditInteraction).
glm::ivec2 forwardAxis(const glm::vec3& direction) {
    const bool alongX = std::abs(direction.x) > std::abs(direction.z);
    return alongX ? glm::ivec2(direction.x > 0.0f ? 1 : -1, 0) : glm::ivec2(0, direction.z > 0.0f ? 1 : -1);
}

/// A nudge's step in the room's frame (millimetres) for "ui_up", "ui_down", "ui_right", "ui_left" seen along `forward`;
/// along its wall only for a hung decoration at `yawCdeg`.
glm::ivec2 nudgeStep(std::string_view action, glm::ivec2 forward, bool hung, std::int32_t yawCdeg) {
    const glm::ivec2 right(-forward.y, forward.x);
    glm::ivec2 step = action == "ui_up"      ? forward
                      : action == "ui_down"  ? -forward
                      : action == "ui_right" ? right
                                             : -right;
    if (hung) {
        step = yawCdeg % 18000 == 0 ? glm::ivec2(step.x, 0) : glm::ivec2(0, step.y);
    }
    return step * static_cast<int>(rv::world::kEditGridMm);
}

/// A movable fixture the reticle can rest on from the eye, where no game or slot is: the point aimed at.
struct FixtureSight {
    rv::world::FixtureId fixture;
    glm::vec3 at{0.0f};
};

/// Points on a fixture's box faces, a few per face, from its middle's height out (a view of it whole first).
std::vector<glm::vec3> facePoints(const rv::core::Aabb& box) {
    std::vector<glm::vec3> points;
    const glm::vec3 low = box.min;
    const glm::vec3 high = box.max;
    for (const float v : {0.5f, 0.75f, 0.25f, 0.08f}) {
        for (const float u : {0.5f, 0.25f, 0.75f}) {
            const float y = low.y + v * (high.y - low.y);
            points.emplace_back(low.x + u * (high.x - low.x), y, high.z);
            points.emplace_back(low.x + u * (high.x - low.x), y, low.z);
            points.emplace_back(high.x, y, low.z + u * (high.z - low.z));
            points.emplace_back(low.x, y, low.z + u * (high.z - low.z));
        }
    }
    return points;
}

/// The planned session: where the walker stands and looks, the fixtures and their places.
struct FurnitureSession {
    rv::app::SpawnPoint spawn;
    /// The poster (a hung decoration) seen at the start, its nudges ("ui_right" or "ui_left") and where it drops.
    rv::world::FixtureId poster;
    std::string posterNudge;
    int posterSteps = 0;
    rv::world::FixtureTransform posterPlace;
    /// The look onto the module, its nudges: `okSteps` to a place it may drop, `badSteps` (more) to one `badRule`
    /// refuses, then back; where it drops.
    rv::world::FixtureId module;
    glm::vec2 toModule{0.0f};
    std::string moduleNudge;
    std::string moduleBack;
    int okSteps = 0;
    int badSteps = 0;
    std::string badRule;
    rv::world::FixtureTransform modulePlace;
    rv::world::FixtureBox moduleBox;
    /// After the save, a start where the walker stood, facing the middle of the module where it stands now, and the walk
    /// into it.
    rv::app::SpawnPoint walkSpawn;
    int walkFrames = 0;
};

/// What the session holds: the poster first (else the walker looks at the module from the start), the refused drop.
struct PlanOptions {
    bool poster = true;
    bool refusal = true;
    /// The most nudges tried in each direction.
    int reach = 12;
};

/// The editor's session planned on `plan`'s room `room` (see the file comment).
FurnitureSession planSession(const fs::path& data, const rv::world::WorldPlan& plan, const rv::world::RoomId& room,
                             const PlanOptions& options = {}) {
    auto sceneData = rv::world::loadSceneData(buildAssets(), plan, room);
    REQUIRE(sceneData.has_value());
    auto layout = rv::world::collectionLayout(plan, room, *sceneData, {});
    REQUIRE(layout.has_value());
    const rv::world::EditData edit = editDataOf(data, plan, room);
    const rv::world::Room& interior = roomIn(plan, room.toString());
    rv::app::EditRoomFrame frame;
    frame.minXMm = interior.minXMm;
    frame.maxXMm = interior.maxXMm;
    frame.minZMm = interior.minZMm;
    frame.maxZMm = interior.maxZMm;
    frame.heightMm = interior.heightMm;
    auto built = rv::app::EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(*layout),
                                          rv::world::editableFixtures(plan, room, edit), frame,
                                          rv::app::fixedFixtureBoxes(plan, room));
    REQUIRE(built.has_value());
    const rv::app::EditRoom& targets = **built;
    const auto boxOf = [](const rv::world::FixtureBox& box) {
        return rv::core::Aabb::fromMinMax(glm::vec3(box.minXMm, box.minYMm, box.minZMm) / 1000.0f,
                                          glm::vec3(box.maxXMm, box.maxYMm, box.maxZMm) / 1000.0f);
    };
    // Where the walker may stand: clear of every occluder, item and slot, inside the room.
    std::vector<rv::core::Aabb> solids = layout->occluders;
    for (const rv::world::CollectionItem& item : layout->items) {
        solids.push_back(item.worldBounds());
    }
    for (const rv::world::FreeSlot& free : layout->freeSlots) {
        solids.push_back(free.worldBounds());
    }
    for (const rv::world::EditableFixture& fixture : targets.fixtures()) {
        solids.push_back(boxOf(fixture.box));
    }
    const auto standable = [&](const glm::vec3& feet, float clear) {
        return std::none_of(solids.begin(), solids.end(), [&](const rv::core::Aabb& box) {
            return box.max.y > feet.y + 0.05f && box.min.y < feet.y + 1.8f && feet.x > box.min.x - clear &&
                   feet.x < box.max.x + clear && feet.z > box.min.z - clear && feet.z < box.max.z + clear;
        });
    };
    // The reticle on a fixture from the eye: no game or slot under it, the fixture the nearest box it enters.
    const auto sight = [&](const glm::vec3& eye,
                           const rv::world::EditableFixture& fixture) -> std::optional<glm::vec3> {
        for (const glm::vec3& point : facePoints(boxOf(fixture.box))) {
            const glm::vec3 to = point - eye;
            if (glm::length(to) > rv::app::kFixturePickDistance - 0.5f || glm::length(to) < 1.5f) {
                continue;
            }
            const rv::core::Ray ray{eye, glm::normalize(to)};
            if (targets.index().sample(ray, rv::world::kNoPick).hit == rv::world::kNoPick &&
                targets.fixtureAt(ray) == fixture.id) {
                return point;
            }
        }
        return std::nullopt;
    };
    // The places a fixture's nudges reach (snapped as the editor snaps them) and whether each drops there.
    const auto nudges = [&](const rv::world::EditedWorld& world, const rv::world::EditableFixture& fixture,
                            std::string_view action, glm::ivec2 forward, int count) {
        std::vector<std::pair<rv::world::FixtureTransform, std::string>> places;
        rv::world::FixtureTransform place = fixture.at;
        for (int step = 0; step < count; ++step) {
            const glm::ivec2 by = nudgeStep(action, forward, fixture.hung, place.yawCdeg);
            const auto snapped = rv::world::snapFixture(
                world, fixture.id, rv::world::FixtureTransform{place.xMm + by.x, place.zMm + by.y, place.yawCdeg},
                edit);
            REQUIRE(snapped.has_value());
            place = *snapped;
            rv::world::EditedWorld trial = world;
            auto result = rv::world::applyEdit(
                trial, rv::world::EditOperation::moveFixture(fixture.id, place.xMm, place.zMm, place.yawCdeg), edit,
                kProvenance);
            REQUIRE(result.has_value());
            places.emplace_back(place, result->refused ? result->refused->rule : std::string());
        }
        return places;
    };

    // The nudges' places of each fixture, by the direction and the view's axis (the floor's validation is slow).
    std::map<std::string, std::vector<std::pair<rv::world::FixtureTransform, std::string>>> nudged;
    const auto nudgesOf = [&](const rv::world::EditedWorld& world, std::string_view worldName,
                              const rv::world::EditableFixture& fixture, std::string_view action, glm::ivec2 forward,
                              int count) -> const std::vector<std::pair<rv::world::FixtureTransform, std::string>>& {
        const std::string key = std::string(worldName) + " " + fixture.id.toString() + " " + std::string(action) + " " +
                                std::to_string(forward.x) + "," + std::to_string(forward.y);
        auto known = nudged.find(key);
        if (known == nudged.end()) {
            known = nudged.emplace(key, nudges(world, fixture, action, forward, count)).first;
        }
        return known->second;
    };

    std::optional<FurnitureSession> found;
    // What the search met (the failure's message).
    std::map<std::string, int> met;
    for (const rv::world::EditableFixture& fixture : targets.fixtures()) {
        ++met[std::string(rv::world::toString(fixture.role)) + (fixture.hung ? " (hung)" : "")];
    }
    const rv::world::EditedWorld start{plan, {}};
    // The module's part from where the walker looks (`look`, at `eye`), on `world` (the poster moved, or not).
    const auto tryModule = [&](const glm::vec3& feet, const glm::vec3& eye, glm::vec2 look,
                               const rv::world::EditedWorld& world, std::string_view worldName,
                               FurnitureSession session) {
        for (const rv::world::EditableFixture& module : targets.fixtures()) {
            if (found) {
                return;
            }
            if (module.role != rv::world::FixtureRole::SideModule || !module.refusal.empty()) {
                continue;
            }
            const std::optional<glm::vec3> moduleAt = sight(eye, module);
            if (!moduleAt) {
                continue;
            }
            ++met["module seen"];
            const glm::vec2 moduleLook = lookAlong(glm::normalize(*moduleAt - eye));
            const glm::ivec2 forward = forwardAxis(*moduleAt - eye);
            for (const auto& [action, back] : {std::pair{"ui_right", "ui_left"}, std::pair{"ui_left", "ui_right"},
                                               std::pair{"ui_down", "ui_up"}, std::pair{"ui_up", "ui_down"}}) {
                if (found) {
                    return;
                }
                const auto& places = nudgesOf(world, worldName, module, action, forward, options.reach);
                for (const auto& [place, rule] : places) {
                    ++met["module nudge " + (rule.empty() ? std::string("ok") : rule)];
                }
                // A place it may drop after 2 to 5 steps (then, with a refusal, a refused one a few steps on).
                for (int ok = 2; ok <= std::min(5, options.reach) && !found; ++ok) {
                    if (!places[ok - 1].second.empty() || places[ok - 1].first == module.at) {
                        continue;
                    }
                    for (int bad = options.refusal ? ok + 1 : ok;
                         bad <= (options.refusal ? std::min(ok + 6, options.reach) : ok) && !found; ++bad) {
                        if (options.refusal && places[bad - 1].second.empty()) {
                            continue;
                        }
                        session.module = module.id;
                        session.toModule = turnPixels(look, moduleLook);
                        session.moduleNudge = action;
                        session.moduleBack = back;
                        session.okSteps = ok;
                        session.badSteps = bad;
                        session.badRule = options.refusal ? places[bad - 1].second : std::string();
                        session.modulePlace = places[ok - 1].first;
                        rv::world::Fixture placed;
                        for (const rv::world::Fixture& fixture : plan.fixtures) {
                            if (fixture.id == module.id) {
                                placed = fixture;
                            }
                        }
                        placed.xMm = session.modulePlace.xMm;
                        placed.zMm = session.modulePlace.zMm;
                        placed.yawCdeg = session.modulePlace.yawCdeg;
                        session.moduleBox = rv::world::fixtureBox(placed);
                        const rv::core::Aabb box = boxOf(session.moduleBox);
                        // Not onto the walker.
                        if (box.inflated(0.5f).contains(glm::vec3(feet.x, box.center().y, feet.z))) {
                            continue;
                        }
                        // The walk into it, the walker turned towards the middle of its box where it stands now:
                        // nothing else within the capsule's reach on the way (what stood on or in front of the module
                        // where it was has moved with it).
                        const glm::vec3 middle(box.center().x, eye.y, box.center().z);
                        const rv::core::Aabb was = boxOf(module.box).inflated(0.05f);
                        const auto inTheWay = [&](const glm::vec3& at) {
                            return std::any_of(solids.begin(), solids.end(), [&](const rv::core::Aabb& solid) {
                                return !solid.intersects(was) && solid.max.y > 0.05f && solid.min.y < 1.8f &&
                                       at.x > solid.min.x - 0.4f && at.x < solid.max.x + 0.4f &&
                                       at.z > solid.min.z - 0.4f && at.z < solid.max.z + 0.4f;
                            });
                        };
                        bool clear = true;
                        const glm::vec3 to(middle.x, 0.0f, middle.z);
                        for (float t = 0.0f; t <= 1.0f && clear; t += 0.02f) {
                            const glm::vec3 at = glm::mix(feet, to, t);
                            if (box.inflated(0.45f).contains(glm::vec3(at.x, box.center().y, at.z))) {
                                break;
                            }
                            clear = !inTheWay(at);
                        }
                        if (!clear) {
                            continue;
                        }
                        const glm::vec2 heading(lookAlong(glm::normalize(middle - eye)).x, 0.0f);
                        session.walkSpawn.feet = feet;
                        session.walkSpawn.yawDeg = rv::core::radiansToDegrees(heading.x);
                        session.walkSpawn.pitchDeg = 0.0f;
                        // Long enough to reach it at the walking speed (2.5 m/s, frames of 1/60 s), and more.
                        session.walkFrames = static_cast<int>(std::ceil(
                            (glm::length(glm::vec2(middle.x - eye.x, middle.z - eye.z)) + 1.0f) * 60.0f / 2.5f));
                        if (!options.poster) {
                            session.spawn.yawDeg = rv::core::radiansToDegrees(moduleLook.x);
                            session.spawn.pitchDeg = rv::core::radiansToDegrees(moduleLook.y);
                            session.toModule = glm::vec2(0.0f);
                        }
                        found = session;
                    }
                }
            }
        }
    };
    for (float x = static_cast<float>(interior.minXMm) / 1000.0f + 1.0f;
         x < static_cast<float>(interior.maxXMm) / 1000.0f - 1.0f && !found; x += 0.25f) {
        for (float z = static_cast<float>(interior.minZMm) / 1000.0f + 1.0f;
             z < static_cast<float>(interior.maxZMm) / 1000.0f - 1.0f && !found; z += 0.25f) {
            const glm::vec3 feet(x, 0.0f, z);
            if (!standable(feet, 0.45f)) {
                continue;
            }
            const glm::vec3 eye = feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
            FurnitureSession session;
            session.spawn.feet = feet;
            if (!options.poster) {
                tryModule(feet, eye, glm::vec2(0.0f), start, "start", session);
                continue;
            }
            for (const rv::world::EditableFixture& poster : targets.fixtures()) {
                if (!poster.hung || !poster.refusal.empty() || found) {
                    continue;
                }
                const std::optional<glm::vec3> posterAt = sight(eye, poster);
                if (!posterAt) {
                    continue;
                }
                ++met["poster seen"];
                const glm::vec2 posterLook = lookAlong(glm::normalize(*posterAt - eye));
                const glm::ivec2 posterForward = forwardAxis(*posterAt - eye);
                // Slid along its wall 2 or 3 steps to a place it may drop.
                for (const std::string_view action : {"ui_right", "ui_left"}) {
                    const auto& places = nudgesOf(start, "start", poster, action, posterForward, 3);
                    for (int steps = 2; steps <= 3 && session.posterSteps == 0; ++steps) {
                        if (places[steps - 1].second.empty() && places[steps - 1].first != poster.at) {
                            session.posterNudge = std::string(action);
                            session.posterSteps = steps;
                            session.posterPlace = places[steps - 1].first;
                        }
                    }
                    if (session.posterSteps > 0) {
                        break;
                    }
                }
                if (session.posterSteps == 0) {
                    continue;
                }
                ++met["poster slid"];
                session.poster = poster.id;
                session.spawn.yawDeg = rv::core::radiansToDegrees(posterLook.x);
                session.spawn.pitchDeg = rv::core::radiansToDegrees(posterLook.y);
                rv::world::EditedWorld afterPoster = start;
                auto moved = rv::world::applyEdit(
                    afterPoster,
                    rv::world::EditOperation::moveFixture(poster.id, session.posterPlace.xMm, session.posterPlace.zMm,
                                                          session.posterPlace.yawCdeg),
                    edit, kProvenance);
                REQUIRE(moved.has_value());
                REQUIRE_FALSE(moved->refused);
                tryModule(feet, eye, posterLook, afterPoster,
                          poster.id.toString() + "@" + std::to_string(session.posterPlace.xMm) + "," +
                              std::to_string(session.posterPlace.zMm),
                          session);
                session.posterSteps = 0;
            }
        }
    }
    std::string counts;
    for (const auto& [what, count] : met) {
        counts += what + ": " + std::to_string(count) + "\n";
    }
    INFO(counts);
    INFO("a place in " << room.toString()
                       << " that sees a poster and a side-wall module where no game is, the poster slid along its "
                          "wall and the module nudged to a place it may drop and one a rule refuses");
    REQUIRE(found.has_value());
    return *found;
}

/// The script of the session (see the file comment); its frames.
struct SessionScript {
    int pickModule = 0;
    int okShot = 0;
    int badDrop = 0;
    int goodDrop = 0;
    int save = 0;
    int walk = 0;
    int frames = 0;
};

SessionScript writeScript(const fs::path& file, const FurnitureSession& session) {
    SessionScript script;
    std::ofstream out(file, std::ios::binary);
    int frame = 0;
    const auto at = [&](int when, const std::string& actions) {
        out << "  {\"fromFrame\": " << when << ", \"toFrame\": " << when << ", \"actions\": {" << actions << "}},\n";
    };
    out << "[\n";
    // The directory's "Modifica stanza" (test_world_edit.cpp's frames).
    at(30, R"("map": 1)");
    at(36, R"("ui_home": 1)");
    at(42, R"("ui_up": 1)");
    at(48, R"("ui_activate": 1)");
    frame = 60;
    if (session.posterSteps > 0) {
        // The poster.
        frame += 10;
        at(frame, R"("interact": 1)");
        for (int step = 0; step < session.posterSteps; ++step) {
            frame += 10;
            at(frame, "\"" + session.posterNudge + "\": 1");
        }
        frame += 10;
        at(frame, R"("interact": 1)");
        // The view onto the module.
        frame += 20;
        out << "  {\"fromFrame\": " << frame << ", \"toFrame\": " << frame
            << ", \"actions\": {\"look_delta_x\": " << session.toModule.x
            << ", \"look_delta_y\": " << session.toModule.y << "}},\n";
    }
    // The module.
    frame += 10;
    script.pickModule = frame;
    at(frame, R"("interact": 1)");
    for (int step = 0; step < session.okSteps; ++step) {
        frame += 10;
        at(frame, "\"" + session.moduleNudge + "\": 1");
    }
    script.okShot = frame + 6;
    if (session.badSteps > session.okSteps) {
        for (int step = session.okSteps; step < session.badSteps; ++step) {
            frame += 10;
            at(frame, "\"" + session.moduleNudge + "\": 1");
        }
        frame += 10;
        script.badDrop = frame;
        at(frame, R"("interact": 1)");
        for (int step = session.okSteps; step < session.badSteps; ++step) {
            frame += 10;
            at(frame, "\"" + session.moduleBack + "\": 1");
        }
    }
    frame += 10;
    script.goodDrop = frame;
    at(frame, R"("interact": 1)");
    // The menu, Salva.
    frame += 20;
    at(frame, R"("confirm": 1)");
    frame += 20;
    script.save = frame;
    out << "  {\"fromFrame\": " << frame << ", \"toFrame\": " << frame << ", \"actions\": {\"ui_activate\": 1}}\n";
    out << "]\n";
    script.frames = script.save + 12;
    return script;
}

/// The walk after the save: from `session`'s walk spawn, straight on into the module (a script of its own).
SessionScript writeWalk(const fs::path& file, const FurnitureSession& session) {
    SessionScript script;
    script.walk = 10;
    std::ofstream out(file, std::ios::binary);
    out << "[\n  {\"fromFrame\": " << script.walk << ", \"toFrame\": " << script.walk + session.walkFrames
        << ", \"actions\": {\"move_forward\": 1}}\n]\n";
    script.frames = script.walk + session.walkFrames + 10;
    return script;
}

std::vector<std::string> sessionArguments(const FurnitureSession& session, int frames, const fs::path& script,
                                          const std::optional<fs::path>& shot, std::string_view room = kRoom) {
    std::vector<std::string> arguments{"--start-room",
                                       std::string(room),
                                       "--exit-after-frames",
                                       "--frames",
                                       std::to_string(frames),
                                       "--fixed-dt",
                                       "0.016667",
                                       "--spawn",
                                       spawnArgument(session.spawn),
                                       "--input-script",
                                       arg(script)};
    if (shot) {
        arguments.insert(arguments.end(), {"--screenshot", arg(*shot)});
    }
    return arguments;
}

const rv::world::Fixture* fixtureIn(const rv::world::WorldPlan& plan, const rv::world::FixtureId& id) {
    const auto found = std::find_if(plan.fixtures.begin(), plan.fixtures.end(),
                                    [&](const rv::world::Fixture& fixture) { return fixture.id == id; });
    return found != plan.fixtures.end() ? &*found : nullptr;
}

} // namespace

TEST_CASE("world_edit: a poster and a side-wall module moved and saved, a refused drop, the room walked after",
          "[world][edit][furniture]") {
    const PlannedBuilding building("world_edit_furniture/session");
    const rv::world::StoredPlan before = building.plan();
    const rv::world::RoomId room = rv::world::RoomId::parse(kRoom).value();
    const FurnitureSession session = planSession(building.data, before.plan, room);
    const fs::path scriptFile = building.dir / "world_edit_furniture.json";
    const SessionScript script = writeScript(scriptFile, session);
    INFO("the poster " << session.poster.toString() << " " << session.posterSteps << " x " << session.posterNudge
                       << ", the module " << session.module.toString() << " " << session.okSteps << " x "
                       << session.moduleNudge << " (refused at " << session.badSteps << ": " << session.badRule << ")");

    // Holding the module where it may drop, and the refused drop with the bar's rule: runs that stop before the save.
    const fs::path holding = building.dir / "holding.png";
    const AppRun holdingRun =
        runApp(retroverseExe(), building.dir, "holding", sessionArguments(session, script.okShot, scriptFile, holding));
    skipWithoutOpenGl(holdingRun);
    requireCleanRun(holdingRun);
    static_cast<void>(requireScreenshot(holdingRun, holding, kShotSize));
    {
        REQUIRE(holdingRun.report->world.edits.size() == 1);
        const rv::app::WorldEditRecord& open = holdingRun.report->world.edits.front();
        CHECK(open.end.empty());
        CHECK(open.operations.size() == 1); // the poster's move
        CHECK(open.probes > 0);
    }
    const fs::path refused = building.dir / "refused.png";
    const AppRun refusedRun = runApp(retroverseExe(), building.dir, "refused",
                                     sessionArguments(session, script.badDrop + 6, scriptFile, refused));
    requireCleanRun(refusedRun);
    static_cast<void>(requireScreenshot(refusedRun, refused, kShotSize));
    {
        REQUIRE(refusedRun.report->world.edits.size() == 1);
        const rv::app::WorldEditRecord& open = refusedRun.report->world.edits.front();
        REQUIRE(open.operations.size() == 2);
        CHECK(open.operations.back().refused == session.badRule);
        CHECK(open.operations.back().path == "none");
    }
    CHECK(building.plan().digest == before.digest);

    // The whole session, saved: the room just after the save.
    const fs::path end = building.dir / "saved.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "session", sessionArguments(session, script.frames, scriptFile, end));
    requireCleanRun(run);
    INFO(run.describe());
    static_cast<void>(requireScreenshot(run, end, kShotSize));
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    CHECK(edit.room == kRoom);
    CHECK(edit.via == "directory");
    CHECK(edit.failed.empty());
    CHECK(edit.end == "saved");
    CHECK(edit.written);
    CHECK_FALSE(edit.replayed);
    REQUIRE(edit.operations.size() == 3);
    const rv::app::EditOperationRecord& poster = edit.operations[0];
    const rv::app::EditOperationRecord& bad = edit.operations[1];
    const rv::app::EditOperationRecord& module = edit.operations[2];
    for (const rv::app::EditOperationRecord* operation : {&poster, &bad, &module}) {
        CHECK(operation->op == "move-fixture");
    }
    CHECK(poster.operation.find(session.poster.toString()) != std::string::npos);
    CHECK(poster.refused.empty());
    CHECK(poster.rebuild == "local-reflow");
    CHECK(poster.path == "room");
    CHECK(bad.refused == session.badRule);
    CHECK(module.operation.find(session.module.toString()) != std::string::npos);
    CHECK(module.refused.empty());
    CHECK(module.path == "room");
    REQUIRE(poster.shownMs.has_value());
    REQUIRE(module.shownMs.has_value());
    UNSCOPED_INFO("the poster's move shown in " << *poster.shownMs << " ms, the module's in " << *module.shownMs
                                                << " ms; saved in " << edit.saveMs.value_or(-1.0)
                                                << " ms, the room back " << edit.backMs.value_or(-1.0) << " ms after");

    // The stored world: both where they were put, pinned by the editor; the module's games stand in its slots.
    const rv::world::StoredPlan after = building.plan();
    CHECK(after.digest != before.digest);
    CHECK(edit.planDigest == after.digest);
    const rv::world::Fixture* posterAfter = fixtureIn(after.plan, session.poster);
    const rv::world::Fixture* moduleAfter = fixtureIn(after.plan, session.module);
    REQUIRE(posterAfter != nullptr);
    REQUIRE(moduleAfter != nullptr);
    CHECK(rv::world::FixtureTransform{posterAfter->xMm, posterAfter->zMm, posterAfter->yawCdeg} == session.posterPlace);
    CHECK(rv::world::FixtureTransform{moduleAfter->xMm, moduleAfter->zMm, moduleAfter->yawCdeg} == session.modulePlace);
    CHECK(before.plan.placements == after.plan.placements);
    auto overrides = rv::world::loadOverrides(
        rv::world::worldFiles(building.data, rv::catalog::LibraryId::parse("esde").value()).overrides);
    REQUIRE(overrides.has_value());
    REQUIRE(overrides->fixturePins.size() == 2);
    for (const rv::world::FixturePin& pin : overrides->fixturePins) {
        CHECK(pin.provenance.origin == rv::world::PinOrigin::Editor);
        CHECK(pin.transform.has_value());
    }

    // The room walked after, started again on the saved plan (nothing written): from where the walker stood, straight
    // into the module, it stops against it where it stands now (its colliders moved with it), never inside it.
    const fs::path walkFile = building.dir / "world_edit_furniture_walk.json";
    const SessionScript walk = writeWalk(walkFile, session);
    const fs::path walked = building.dir / "walked.png";
    const AppRun walkRun = runApp(retroverseExe(), building.dir, "walk",
                                  {"--start-room", std::string(kRoom), "--exit-after-frames", "--frames",
                                   std::to_string(walk.frames), "--fixed-dt", "0.016667", "--spawn",
                                   spawnArgument(session.walkSpawn), "--input-script", arg(walkFile), "--screenshot",
                                   arg(walked)});
    requireCleanRun(walkRun);
    INFO(walkRun.describe());
    static_cast<void>(requireScreenshot(walkRun, walked, kShotSize));
    CHECK_FALSE(walkRun.report->world.planWritten);
    CHECK(walkRun.report->world.planDigest == after.digest);
    CHECK(walkRun.report->world.edits.empty());
    REQUIRE(walkRun.report->finalCamera.has_value());
    const glm::vec3 feet = walkRun.report->finalCamera->feet;
    const auto metres = [](std::int64_t millimetres) {
        return static_cast<float>(millimetres) / 1000.0f;
    };
    const float dx =
        std::max({metres(session.moduleBox.minXMm) - feet.x, feet.x - metres(session.moduleBox.maxXMm), 0.0f});
    const float dz =
        std::max({metres(session.moduleBox.minZMm) - feet.z, feet.z - metres(session.moduleBox.maxZMm), 0.0f});
    const float gap = std::sqrt(dx * dx + dz * dz);
    INFO("the walker's feet at " << feet.x << ", " << feet.z << ": " << gap << " m from the module's box");
    CHECK(gap >= kRadius - 0.06f);
    CHECK(gap <= kRadius + 0.15f);
}

// ADR-0015 §9's rows for a fixture moved and the save (the room's static part rebuilt alone: I4b step 4), on the real
// library's computer floor, measured by hand in Release (hidden: [.real-library]): only through
// scripts/real-library.ps1 (ES-DE closed, read-only snapshots of the library before and after), its -Measure script
// running rv_smoke_tests.exe "[real-library][edit]" with the RV_REAL_* variables set. The library imported into a data
// directory under the build tree (build/<preset>/real-library/furniture, emptied first), its building generated and
// shown once; then, in the computer floor's room with the most placements where a side-wall module can be picked up
// and moved a few grid steps, a session moves it and saves: the move's job and display times, the save's and the
// room's back printed.
TEST_CASE("world_edit: a fixture moved and saved on the real library's computer floor",
          "[.real-library][edit][furniture]") {
    const auto home = rv::test::environmentVariable(L"RV_REAL_ESDE_HOME");
    const auto install = rv::test::environmentVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = rv::test::environmentVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = rv::test::environmentVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    // Under the build tree, never in the library nor in %TEMP%.
    const fs::path dir = retroverseExe().parent_path().parent_path() / "real-library" / "furniture";
    std::error_code error;
    fs::remove_all(dir, error);
    REQUIRE_FALSE(error);
    fs::create_directories(dir, error);
    REQUIRE_FALSE(error);
    const fs::path data = dir / "data";
    const auto cli = [&](std::string_view name, std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseExe().parent_path() / "retroverse-cli.exe", dir,
                                      std::string("cli_") + std::string(name), std::move(arguments));
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    };
    cli("library", {"library", "create", "--home", arg(fs::path(*home).parent_path()), "--install", arg(*install),
                    "--rom-root", arg(*romRoot), "--media-root", arg(*mediaRoot)});
    cli("import", {"import"});
    cli("generate", {"worldplan", "generate"});
    const rv::world::WorldFiles files = rv::world::worldFiles(data, rv::catalog::LibraryId::parse("esde").value());
    auto stored = rv::world::readStoredPlan(files.plan);
    REQUIRE(stored.has_value());
    const rv::world::WorldPlan& plan = stored->plan;

    // The computer floor (else the floor with the most placements), its rooms by their placements.
    std::map<std::string, std::size_t> placed;
    std::map<std::string, std::size_t> floorPlaced;
    for (const rv::world::Placement& placement : plan.placements) {
        ++placed[placement.slot.fixture().room().toString()];
    }
    for (const rv::world::Room& room : plan.rooms) {
        floorPlaced[room.floor] += placed[room.id.toString()];
    }
    REQUIRE_FALSE(floorPlaced.empty());
    const std::string floor =
        floorPlaced.contains("computer")
            ? std::string("computer")
            : std::max_element(floorPlaced.begin(), floorPlaced.end(), [](const auto& a, const auto& b) {
                  return a.second < b.second;
              })->first;
    std::vector<const rv::world::Room*> rooms;
    for (const rv::world::Room& room : plan.rooms) {
        if (room.floor == floor && placed[room.id.toString()] > 0) {
            rooms.push_back(&room);
        }
    }
    std::sort(rooms.begin(), rooms.end(), [&](const rv::world::Room* a, const rv::world::Room* b) {
        return placed[a->id.toString()] > placed[b->id.toString()];
    });
    PlanOptions options;
    options.poster = false;
    options.refusal = false;
    options.reach = 5;
    std::optional<FurnitureSession> session;
    std::string room;
    for (const rv::world::Room* candidate : rooms) {
        const bool hasModule = std::any_of(plan.fixtures.begin(), plan.fixtures.end(), [&](const auto& fixture) {
            return fixture.id.room() == candidate->id && fixture.role == rv::world::FixtureRole::SideModule;
        });
        if (!hasModule) {
            continue;
        }
        session = planSession(data, plan, candidate->id, options);
        room = candidate->id.toString();
        break;
    }
    INFO("a side-wall module to move on floor " << floor);
    REQUIRE(session.has_value());

    // The building shown once (its covers into the cache), then the session: the module moved, Salva.
    const AppRun warm = runApp(retroverseExe(), dir, "warm",
                               {"--start-room", room, "--exit-after-frames", "--frames", "300", "--fixed-dt",
                                "0.016667", "--spawn", spawnArgument(session->spawn)});
    skipWithoutOpenGl(warm);
    requireCleanRun(warm);
    const fs::path scriptFile = dir / "world_edit_furniture_real.json";
    const SessionScript script = writeScript(scriptFile, *session);
    const AppRun run =
        runApp(retroverseExe(), dir, "session", sessionArguments(*session, script.frames, scriptFile, {}, room));
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    REQUIRE(edit.operations.size() == 1);
    const rv::app::EditOperationRecord& move = edit.operations.front();
    WARN("Real furniture: " << room << " (" << placed[room] << " placed) on floor " << floor << " ("
                            << floorPlaced[floor] << " placed, " << plan.placements.size()
                            << " in the building): the session ready in " << edit.prepareMs.value_or(-1.0) << " ms; "
                            << move.operation << ": " << move.rebuild << ", " << move.path << ", the job "
                            << move.editMs.value_or(-1.0) << " ms, shown " << move.shownMs.value_or(-1.0)
                            << " ms after the request; " << edit.probes << " place(s) judged; saved in "
                            << edit.saveMs.value_or(-1.0) << " ms, the room back " << edit.backMs.value_or(-1.0)
                            << " ms after Salva");
    CHECK(move.refused.empty());
    CHECK(move.path == "room");
    CHECK(edit.end == "saved");
    CHECK(edit.written);
}

// ADR-0015 §9's row for a fixture moved, measured by hand in Release (hidden: [.bench]): the overflow fixture's amiga
// room (1,561 big boxes, the size of the reference library's largest room) on its floor, a side-wall module picked up,
// nudged a few grid steps and dropped (its room's static part rebuilt alone, its collection laid out again), then Salva:
// the move's and the save's times printed. Run: rv_smoke_tests.exe "[.bench][furniture]" -s.
TEST_CASE("world_edit: a fixture moved in a room of 1,561 big boxes", "[.bench][edit][furniture]") {
    const PlannedBuilding building("world_edit_furniture/bench", true, "esde/overflow");
    const rv::world::StoredPlan stored = building.plan();
    const std::string room = building.roomOf("amiga");
    PlanOptions options;
    options.poster = false;
    options.refusal = false;
    options.reach = 5;
    const FurnitureSession session =
        planSession(building.data, stored.plan, rv::world::RoomId::parse(room).value(), options);
    const fs::path scriptFile = building.dir / "world_edit_furniture_bench.json";
    const SessionScript script = writeScript(scriptFile, session);
    const fs::path shot = building.dir / "bench.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "bench",
                              sessionArguments(session, script.frames, scriptFile, shot, room));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    for (const rv::app::EditOperationRecord& operation : edit.operations) {
        WARN(operation.operation << ": " << operation.path << ", the job " << operation.editMs.value_or(-1.0)
                                 << " ms, shown " << operation.shownMs.value_or(-1.0) << " ms after the request");
    }
    WARN("the room's data and floor read in " << edit.prepareMs.value_or(-1.0) << " ms; " << edit.probes
                                              << " place(s) judged; saved in " << edit.saveMs.value_or(-1.0)
                                              << " ms, the room back " << edit.backMs.value_or(-1.0)
                                              << " ms after Salva");
    CHECK(edit.end == "saved");
    REQUIRE(edit.operations.size() == 1);
    CHECK(edit.operations.front().path == "room");
}
