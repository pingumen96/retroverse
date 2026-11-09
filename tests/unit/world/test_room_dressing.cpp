// I12 step 1 (ADR-0021 §1): the pack's architecture and pendants in a plan's room (room_look.hpp's
// "roomDressing.shell" and "roomDressing.pendants", drawn by toSceneFile as "dressing:" primitives): the reader, the
// shipped packs, every golden plan's room, the look pendant's light among the scene's.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <set>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <rv/assets/material_library.hpp>
#include <rv/world/gallery.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_plan.hpp>

#include "plan_scene_support.hpp"

namespace {

using Catch::Matchers::ContainsSubstring;
using nlohmann::json;
using rv::world::FixtureRole;

const std::filesystem::path& assets() {
    return rv::test::world::sourceAssetRoot();
}

double meters(std::int64_t millimetres) {
    return static_cast<double>(millimetres) / 1000.0;
}

bool near(double a, double b) {
    return std::abs(a - b) < 1e-6;
}

/// A dressing primitive's extent in the room's frame (meters): a box's size or a cylinder's radius and height about its
/// position.
struct Extent {
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
    double minZ = 0.0;
    double maxZ = 0.0;
};

Extent extentOf(const json& object) {
    const json& position = object.at("position");
    const json& params = object.at("params");
    double halfX = 0.0;
    double halfY = 0.0;
    double halfZ = 0.0;
    if (object.at("primitive") == "box") {
        halfX = params.at("size")[0].get<double>() / 2.0;
        halfY = params.at("size")[1].get<double>() / 2.0;
        halfZ = params.at("size")[2].get<double>() / 2.0;
    } else {
        REQUIRE(object.at("primitive") == "cylinder");
        halfX = params.at("radius").get<double>();
        halfZ = halfX;
        halfY = params.at("height").get<double>() / 2.0;
    }
    const double x = position[0].get<double>();
    const double y = position[1].get<double>();
    const double z = position[2].get<double>();
    return Extent{x - halfX, x + halfX, y - halfY, y + halfY, z - halfZ, z + halfZ};
}

/// The volume two extents share (0 when they only touch).
double sharedVolume(const Extent& a, const Extent& b) {
    const double x = std::min(a.maxX, b.maxX) - std::max(a.minX, b.minX);
    const double y = std::min(a.maxY, b.maxY) - std::max(a.minY, b.minY);
    const double z = std::min(a.maxZ, b.maxZ) - std::max(a.minZ, b.minZ);
    return x > 1e-9 && y > 1e-9 && z > 1e-9 ? x * y * z : 0.0;
}

std::vector<json> withPrefix(const json& scene, std::string_view prefix) {
    std::vector<json> found;
    for (const json& object : scene.at("objects")) {
        if (object.at("id").get<std::string>().starts_with(prefix)) {
            found.push_back(object);
        }
    }
    return found;
}

const json& objectNamed(const json& scene, const std::string& id) {
    const json& objects = scene.at("objects");
    const auto found = std::find_if(objects.begin(), objects.end(),
                                    [&](const json& object) { return object.at("id").get<std::string>() == id; });
    INFO(id);
    REQUIRE(found != objects.end());
    return *found;
}

bool hasLight(const json& scene, std::string_view name) {
    const json& lights = scene.at("lights");
    return std::any_of(lights.begin(), lights.end(),
                       [&](const json& light) { return light.at("name").get<std::string>() == name; });
}

json sceneOf(const rv::world::WorldPlan& plan, const rv::world::RoomId& room, const rv::world::SceneData& data) {
    auto build = rv::world::toSceneFile(plan, room, data);
    INFO((build ? std::string("built") : build.error().toString()));
    REQUIRE(build.has_value());
    return build->sceneFile;
}

/// The beams' z in the room's frame (meters).
std::vector<double> beamCentres(const json& scene) {
    std::vector<double> centres;
    for (const json& beam : withPrefix(scene, "dressing:shell/beam:")) {
        const Extent extent = extentOf(beam);
        centres.push_back((extent.minZ + extent.maxZ) / 2.0);
    }
    return centres;
}

} // namespace

TEST_CASE("world/room_dressing: the reader takes a pack's architecture and pendants, refusing a broken one",
          "[world][room_dressing]") {
    const auto parse = [](const std::string& dressing) {
        return rv::world::parseThemeMaterials(
            R"({"schemaVersion": 1, "id": "t", "name": "T", "heroParts": {}, "kinds": {}, "roomDressing": )" +
                dressing + "}",
            "t.json");
    };
    const auto refusal = [&](const std::string& dressing) {
        auto parsed = parse(dressing);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error().message;
    };
    auto good = parse(R"({
        "shell": {"railYMm": 2650, "rail": "MAT_Wood_Oak", "wallpaper": "MAT_Wallpaper_Cream",
                  "frieze": "MAT_Plaster_Warm", "cornice": "MAT_Wood_Oak", "beams": "MAT_Wood_Walnut",
                  "beamPitchMm": 2500},
        "pendants": {"shade": "MAT_Pendant_Shade", "diffuser": "MAT_Pendant_Diffuser", "metal": "MAT_Metal_Brass"}})");
    REQUIRE(good.has_value());
    REQUIRE(good->dressing.shell.has_value());
    const rv::world::ShellDressing& shell = *good->dressing.shell;
    CHECK(shell.railYMm == 2650);
    CHECK(shell.rail == "MAT_Wood_Oak");
    CHECK(shell.wallpaper == "MAT_Wallpaper_Cream");
    CHECK(shell.frieze == "MAT_Plaster_Warm");
    CHECK(shell.cornice == "MAT_Wood_Oak");
    CHECK(shell.beams == "MAT_Wood_Walnut");
    CHECK(shell.beamPitchMm == 2500);
    REQUIRE(good->dressing.pendants.has_value());
    CHECK(good->dressing.pendants->shade == "MAT_Pendant_Shade");
    CHECK(good->dressing.pendants->diffuser == "MAT_Pendant_Diffuser");
    CHECK(good->dressing.pendants->metal == "MAT_Metal_Brass");
    CHECK_FALSE(good->dressing.empty());

    // Every part of the architecture optional (the arcade's cornice alone), the rail 2.7 m up and a beam every 3 m
    // unless the pack says otherwise; the pendants the pack's own choice.
    auto cornice = parse(R"({"shell": {"cornice": "MAT_Metal_Graphite"}})");
    REQUIRE(cornice.has_value());
    REQUIRE(cornice->dressing.shell.has_value());
    CHECK(cornice->dressing.shell->railYMm == 2700);
    CHECK(cornice->dressing.shell->beamPitchMm == 3000);
    CHECK(cornice->dressing.shell->rail.empty());
    CHECK(cornice->dressing.shell->wallpaper.empty());
    CHECK(cornice->dressing.shell->beams.empty());
    CHECK_FALSE(cornice->dressing.pendants.has_value());
    CHECK_FALSE(cornice->dressing.empty());

    CHECK_THAT(refusal(R"({"shell": {"frieze": "MAT_Plaster_Warm"}})"), ContainsSubstring("rail"));
    CHECK_THAT(refusal(R"({"shell": {"railYMm": 1200, "rail": "MAT_Wood_Oak"}})"), ContainsSubstring("railYMm"));
    CHECK_THAT(refusal(R"({"shell": {"beamPitchMm": 500, "beams": "MAT_Wood_Oak"}})"),
               ContainsSubstring("beamPitchMm"));
    CHECK_THAT(refusal(R"({"shell": {"skirting": "MAT_Wood_Oak"}})"), ContainsSubstring("skirting"));
    CHECK_THAT(refusal(R"({"shell": {"wallpaper": "damask"}})"), ContainsSubstring("MAT_"));
    CHECK_THAT(refusal(R"({"pendants": {"shade": "MAT_Pendant_Shade", "diffuser": "MAT_Pendant_Diffuser"}})"),
               ContainsSubstring("metal"));
}

TEST_CASE("world/room_dressing: the shipped packs' architecture and pendants name library materials, the arcade's a "
          "cornice alone",
          "[world][room_dressing]") {
    auto library = rv::assets::MaterialLibrary::loadFromFile(assets() / "materials" / "base" / "materials.json");
    REQUIRE(library.has_value());
    for (const std::string& id : rv::world::themePackIds(assets())) {
        INFO(id);
        auto theme = rv::world::loadThemeMaterials(assets(), id);
        REQUIRE(theme.has_value());
        const rv::world::RoomDressing& dressing = theme->dressing;
        std::vector<std::string> materials;
        if (dressing.shell) {
            materials.insert(materials.end(), {dressing.shell->rail, dressing.shell->wallpaper, dressing.shell->frieze,
                                               dressing.shell->cornice, dressing.shell->beams});
        }
        if (dressing.pendants) {
            materials.insert(materials.end(),
                             {dressing.pendants->shade, dressing.pendants->diffuser, dressing.pendants->metal});
        }
        for (const std::string& material : materials) {
            if (!material.empty()) {
                INFO(material);
                CHECK(library->find(material) != nullptr);
            }
        }
        if (id == "retro_library_hub") {
            // The hub's pack dresses its halls (floor_scene.hpp), not a room.
            CHECK_FALSE(dressing.shell.has_value());
            CHECK_FALSE(dressing.pendants.has_value());
        } else if (id == "arcade_after_dark") {
            // The arcade's dark ceiling over its coves: a graphite cornice, no paper, rail, beam or pendant.
            REQUIRE(dressing.shell.has_value());
            CHECK(dressing.shell->cornice == "MAT_Metal_Graphite");
            CHECK(dressing.shell->wallpaper.empty());
            CHECK(dressing.shell->rail.empty());
            CHECK(dressing.shell->beams.empty());
            CHECK_FALSE(dressing.pendants.has_value());
        } else {
            // Every other room's pack papers its walls up to a picture rail, caps them with a cornice, beams its
            // ceiling and hangs its pendants.
            REQUIRE(dressing.shell.has_value());
            CHECK_FALSE(dressing.shell->wallpaper.empty());
            CHECK_FALSE(dressing.shell->rail.empty());
            CHECK_FALSE(dressing.shell->cornice.empty());
            CHECK_FALSE(dressing.shell->beams.empty());
            CHECK(dressing.pendants.has_value());
        }
    }
    // The built-in pack is the classic one's copy (test_room_look.cpp), its architecture and pendants with it.
    auto classic = rv::world::loadThemeMaterials(assets(), "classic_16bit_warm");
    REQUIRE(classic.has_value());
    CHECK(rv::world::builtInThemeMaterials().dressing == classic->dressing);
}

TEST_CASE("world/room_dressing: every golden plan's room papered to its rail, corniced and beamed, the doorway's wall "
          "around its casing",
          "[world][room_dressing]") {
    for (const std::string& name : rv::test::world::goldenPlanNames()) {
        INFO(name);
        const rv::test::world::PlanCase& entry = rv::test::world::planCase(name);
        const rv::world::Room& room = entry.plan.rooms.front();
        const rv::world::Doorway& door = entry.data.roomTemplate.doorway;
        REQUIRE(door.wall == rv::world::Wall::South);
        REQUIRE(entry.data.theme.dressing.shell.has_value());
        const rv::world::ShellDressing& shell = *entry.data.theme.dressing.shell;
        const json scene = sceneOf(entry.plan, entry.room, entry.data);
        const double minX = meters(room.minXMm);
        const double maxX = meters(room.maxXMm);
        const double minZ = meters(room.minZMm);
        const double maxZ = meters(room.maxZMm);
        const double height = meters(room.heightMm);

        // Every piece within the room, neither colliding nor casting a shadow.
        for (const json& piece : withPrefix(scene, "dressing:shell/")) {
            INFO(piece.at("id").get<std::string>());
            CHECK(piece.at("collides") == false);
            CHECK(piece.at("castsShadows") == false);
            const Extent extent = extentOf(piece);
            CHECK(extent.minX >= minX - 1e-6);
            CHECK(extent.maxX <= maxX + 1e-6);
            CHECK(extent.minY >= -1e-6);
            CHECK(extent.maxY <= height + 1e-6);
            CHECK(extent.minZ >= minZ - 1e-6);
            CHECK(extent.maxZ <= maxZ + 1e-6);
        }

        // The bands: the paper from the wainscot's top to the rail, the rail, the cornice under the ceiling. Each piece
        // against its wall, the band's depth proud of it; the bands of one height meet at the corners without
        // overlapping; on the doorway's wall the opening and its casing stay clear, the paper beside and over them.
        const double railBottom = meters(shell.railYMm - 45);
        const double doorTop = meters(door.heightMm + 70);
        const double gapFrom = meters(door.offsetMm - door.widthMm / 2 - 70);
        const double gapTo = meters(door.offsetMm + door.widthMm / 2 + 70);
        struct Band {
            std::string_view prefix;
            double bottom = 0.0;
            double top = 0.0;
            double depth = 0.0;
            std::size_t pieces = 0;
        };
        for (const Band& band : {Band{"dressing:shell/wallpaper-", 1.0, railBottom, 0.005, 6},
                                 Band{"dressing:shell/rail-", railBottom, meters(shell.railYMm), 0.028, 4},
                                 Band{"dressing:shell/cornice-", height - 0.09, height, 0.06, 4}}) {
            INFO(band.prefix);
            const std::vector<json> pieces = withPrefix(scene, band.prefix);
            CHECK(pieces.size() == band.pieces);
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                const std::string id = pieces[i].at("id");
                INFO(id);
                const Extent extent = extentOf(pieces[i]);
                CHECK(near(extent.minY, id.ends_with("/over") ? std::max(band.bottom, doorTop) : band.bottom));
                CHECK(near(extent.maxY, band.top));
                const std::size_t wallEnd = id.find('/', band.prefix.size());
                const std::string wall =
                    id.substr(band.prefix.size(),
                              wallEnd == std::string::npos ? std::string::npos : wallEnd - band.prefix.size());
                if (wall == "north") {
                    CHECK(near(extent.minZ, minZ));
                    CHECK(near(extent.maxZ - extent.minZ, band.depth));
                } else if (wall == "south") {
                    CHECK(near(extent.maxZ, maxZ));
                    CHECK(near(extent.maxZ - extent.minZ, band.depth));
                    CHECK((extent.maxX <= gapFrom + 1e-6 || extent.minX >= gapTo - 1e-6 ||
                           extent.minY >= doorTop - 1e-6));
                } else if (wall == "west") {
                    CHECK(near(extent.minX, minX));
                    CHECK(near(extent.maxX - extent.minX, band.depth));
                } else {
                    CHECK(wall == "east");
                    CHECK(near(extent.maxX, maxX));
                    CHECK(near(extent.maxX - extent.minX, band.depth));
                }
                for (std::size_t j = i + 1; j < pieces.size(); ++j) {
                    CHECK(sharedVolume(extent, extentOf(pieces[j])) == 0.0);
                }
            }
        }
        const Extent beside = extentOf(objectNamed(scene, "dressing:shell/wallpaper-south/a"));
        const Extent besideToo = extentOf(objectNamed(scene, "dressing:shell/wallpaper-south/b"));
        const Extent over = extentOf(objectNamed(scene, "dressing:shell/wallpaper-south/over"));
        CHECK(near(beside.minX, minX));
        CHECK(near(beside.maxX, gapFrom));
        CHECK(near(besideToo.minX, gapTo));
        CHECK(near(besideToo.maxX, maxX));
        CHECK(near(over.minX, gapFrom));
        CHECK(near(over.maxX, gapTo));
        // The paper's metres anchored at each piece's place: the pieces of a wall tile on without a seam.
        for (const json& paper : withPrefix(scene, "dressing:shell/wallpaper-")) {
            CHECK(paper.at("params").at("uv") == "meters");
            CHECK(paper.at("params").at("uvOrigin") == paper.at("position"));
            CHECK(paper.at("material") == shell.wallpaper);
        }

        // The beams: every pitch from the north wall while a metre short of the south one, wall to wall under the
        // ceiling.
        const std::vector<json> beams = withPrefix(scene, "dressing:shell/beam:");
        CHECK(beams.size() == static_cast<std::size_t>((room.maxZMm - room.minZMm - 1000) / shell.beamPitchMm));
        for (std::size_t k = 0; k < beams.size(); ++k) {
            const std::int64_t along = shell.beamPitchMm * static_cast<std::int64_t>(k + 1);
            CHECK(beams[k].at("id") == "dressing:shell/beam:" + std::to_string(along));
            CHECK(beams[k].at("material") == shell.beams);
            const Extent extent = extentOf(beams[k]);
            CHECK(near(extent.minX, minX));
            CHECK(near(extent.maxX, maxX));
            CHECK(near(extent.maxY, height));
            CHECK(near(extent.minY, height - 0.2));
            CHECK(near((extent.minZ + extent.maxZ) / 2.0, meters(room.minZMm + along)));
        }
    }
}

TEST_CASE("world/room_dressing: the pendants near the doorway and over the free-standing display cases, the look "
          "pendant's light last in a slot left free",
          "[world][room_dressing]") {
    std::size_t displayLamps = 0;
    std::size_t lookLights = 0;
    for (const std::string& name : rv::test::world::goldenPlanNames()) {
        INFO(name);
        const rv::test::world::PlanCase& entry = rv::test::world::planCase(name);
        const rv::world::Room& room = entry.plan.rooms.front();
        const rv::world::Doorway& door = entry.data.roomTemplate.doorway;
        REQUIRE(entry.data.theme.dressing.pendants.has_value());
        const rv::world::PendantDressing& lamps = *entry.data.theme.dressing.pendants;
        const json scene = sceneOf(entry.plan, entry.room, entry.data);
        const double height = meters(room.heightMm);
        const std::vector<double> beams = beamCentres(scene);

        // Every part of a lamp neither collides nor casts a shadow.
        for (const json& piece : withPrefix(scene, "dressing:pendant:")) {
            INFO(piece.at("id").get<std::string>());
            CHECK(piece.at("collides") == false);
            CHECK(piece.at("castsShadows") == false);
        }
        const auto underBeam = [&](double z, double reach) {
            return std::any_of(beams.begin(), beams.end(),
                               [&](double beam) { return std::abs(beam - z) <= 0.09 + reach + 1e-6; });
        };

        // The drum near the doorway on its axis, 1.5 m into the room, its bottom 1.12 m under the ceiling: hung from
        // the ceiling, or from the underside of a beam passing over it, the stem from the drum's top to the canopy.
        const std::string doorway = "dressing:pendant:doorway";
        const json& drumObject = objectNamed(scene, doorway + "/drum");
        CHECK(drumObject.at("material") == lamps.shade);
        CHECK(drumObject.at("params").at("capped") == false);
        CHECK(objectNamed(scene, doorway + "/diffuser").at("material") == lamps.diffuser);
        CHECK(objectNamed(scene, doorway + "/top").at("material") == lamps.metal);
        const Extent drum = extentOf(drumObject);
        CHECK(near((drum.minX + drum.maxX) / 2.0, meters(door.offsetMm)));
        CHECK(near((drum.minZ + drum.maxZ) / 2.0, meters(room.maxZMm - 1500)));
        CHECK(near(drum.minY, meters(room.heightMm - 1120)));
        CHECK(near(drum.maxY - drum.minY, 0.22));
        const Extent canopy = extentOf(objectNamed(scene, doorway + "/canopy"));
        const Extent drumStem = extentOf(objectNamed(scene, doorway + "/stem"));
        CHECK(near(canopy.maxY, underBeam(meters(room.maxZMm - 1500), 0.055) ? height - 0.2 : height));
        CHECK(near(drumStem.minY, drum.maxY));
        CHECK(near(drumStem.maxY, canopy.minY));

        // A spot can over each display case standing free (never over the wall shelf, which hangs on the hero wall):
        // its lens 2.35 m up under the can, its stem up to the ceiling or a beam's underside.
        std::size_t cans = 0;
        for (const rv::world::Fixture& fixture : entry.plan.fixtures) {
            if (fixture.role != FixtureRole::Display) {
                continue;
            }
            const std::string id = "dressing:pendant:" + fixture.id.localText();
            INFO(id);
            if (fixture.prefab.kind != "display_case") {
                CHECK(withPrefix(scene, id + "/").empty());
                continue;
            }
            ++cans;
            const json& lensObject = objectNamed(scene, id + "/lens");
            const json& canObject = objectNamed(scene, id + "/can");
            CHECK(lensObject.at("material") == lamps.diffuser);
            CHECK(canObject.at("material") == lamps.metal);
            const Extent lens = extentOf(lensObject);
            const Extent can = extentOf(canObject);
            const Extent stem = extentOf(objectNamed(scene, id + "/stem"));
            CHECK(near((can.minX + can.maxX) / 2.0, meters(fixture.xMm)));
            CHECK(near((can.minZ + can.maxZ) / 2.0, meters(fixture.zMm)));
            CHECK(near(lens.minY, 2.35));
            CHECK(near(can.minY, lens.maxY));
            CHECK(near(stem.minY, can.maxY));
            CHECK(near(stem.maxY, underBeam(meters(fixture.zMm), 0.006) ? height - 0.2 : height));
            CHECK(lens.maxX - lens.minX < can.maxX - can.minX);
        }
        displayLamps += cans;
        CHECK(withPrefix(scene, "dressing:pendant:").size() == 5 + 3 * cans);

        // The look pendant's light: a point inside the doorway's drum after the plan's lights and every accent, the
        // profile's pendant colour at half its intensity, casting no shadow; none where those fill the scene's limit
        // (the large rooms': their drum lit by its own glow alone).
        const json& lights = scene.at("lights");
        const std::size_t planLights = entry.plan.lights.size();
        CHECK(lights.size() <= rv::world::kMaxSceneLights);
        for (std::size_t i = planLights; i < lights.size(); ++i) {
            const std::string light = lights[i].at("name");
            CHECK((light.starts_with("accent:") || (light == "look:pendant" && i + 1 == lights.size())));
        }
        if (!hasLight(scene, "look:pendant")) {
            CHECK(lights.size() == rv::world::kMaxSceneLights);
            continue;
        }
        ++lookLights;
        const json& look = lights.back();
        CHECK(look.at("name") == "look:pendant");
        CHECK(look.at("type") == "point");
        CHECK(look.value("castsShadows", false) == false);
        CHECK(near(look.at("position")[0].get<double>(), meters(door.offsetMm)));
        CHECK(near(look.at("position")[1].get<double>(), meters(room.heightMm - 1100)));
        CHECK(near(look.at("position")[2].get<double>(), meters(room.maxZMm - 1500)));
        CHECK(look.at("color") == entry.data.lighting.pendant.color);
        CHECK(std::abs(look.at("intensity").get<double>() - entry.data.lighting.pendant.intensity * 0.5) < 1e-3);
    }
    // The golden plans' rooms hold display cases (all but the empty ones) and leave the look pendant a slot (the
    // smaller ones): their lamps and lights were judged.
    CHECK(displayLamps > 0);
    CHECK(lookLights > 0);
}

TEST_CASE("world/room_dressing: the look pendant's light only while the scene's limit leaves room for it and with the "
          "pack's pendants",
          "[world][room_dressing]") {
    // A room of 57 titles: ten plan lights, five accents and the look pendant's light, the scene's sixteen.
    const rv::test::world::PlanCase& entry = rv::test::world::planCase("plan-57-seed-102938");
    const json dressed = sceneOf(entry.plan, entry.room, entry.data);
    const double height = meters(entry.plan.rooms.front().heightMm);
    REQUIRE(dressed.at("lights").size() == rv::world::kMaxSceneLights);
    REQUIRE(dressed.at("lights").back().at("name") == "look:pendant");
    std::vector<std::string> accents;
    for (const json& light : dressed.at("lights")) {
        if (light.at("name").get<std::string>().starts_with("accent:")) {
            accents.push_back(light.at("name").get<std::string>());
        }
    }
    REQUIRE_FALSE(accents.empty());

    // One plan light more: every accent still lit, no slot left for the look pendant (its drum hung all the same).
    rv::world::WorldPlan more = entry.plan;
    const auto pendant = std::find_if(more.lights.begin(), more.lights.end(), [](const rv::world::PlanLight& light) {
        return light.role == rv::world::LightRole::Pendant;
    });
    REQUIRE(pendant != more.lights.end());
    const rv::world::PlanLight model = *pendant;
    const auto addLight = [&](rv::world::WorldPlan& plan) {
        auto id = rv::world::FixtureId::make(entry.room, "room", "fill" + std::to_string(plan.lights.size()));
        REQUIRE(id.has_value());
        rv::world::PlanLight extra = model;
        extra.id = *id;
        plan.lights.push_back(extra);
    };
    addLight(more);
    const json crowded = sceneOf(more, entry.room, entry.data);
    CHECK(crowded.at("lights").size() == rv::world::kMaxSceneLights);
    CHECK_FALSE(hasLight(crowded, "look:pendant"));
    for (const std::string& accent : accents) {
        INFO(accent);
        CHECK(hasLight(crowded, accent));
    }
    CHECK(withPrefix(crowded, "dressing:pendant:doorway/").size() == 5);
    // A plan of sixteen lights: none more, neither accent nor look pendant.
    while (more.lights.size() < rv::world::kMaxSceneLights) {
        addLight(more);
    }
    const json full = sceneOf(more, entry.room, entry.data);
    CHECK(full.at("lights").size() == rv::world::kMaxSceneLights);
    CHECK_FALSE(hasLight(full, "look:pendant"));
    CHECK_FALSE(hasLight(full, accents.front()));

    // A pack without pendants: no lamp and no look pendant's light, the architecture and the other lights as they were.
    rv::world::SceneData data = entry.data;
    data.theme.dressing.pendants.reset();
    const json unlit = sceneOf(entry.plan, entry.room, data);
    CHECK(withPrefix(unlit, "dressing:pendant:").empty());
    CHECK_FALSE(hasLight(unlit, "look:pendant"));
    CHECK(withPrefix(unlit, "dressing:shell/").size() == withPrefix(dressed, "dressing:shell/").size());
    REQUIRE(unlit.at("lights").size() + 1 == dressed.at("lights").size());
    for (std::size_t i = 0; i < unlit.at("lights").size(); ++i) {
        CHECK(unlit.at("lights")[i] == dressed.at("lights")[i]);
    }
    // Without the architecture: no band and no beam, every lamp hung from the ceiling.
    data = entry.data;
    data.theme.dressing.shell.reset();
    const json bare = sceneOf(entry.plan, entry.room, data);
    CHECK(withPrefix(bare, "dressing:shell/").empty());
    CHECK(withPrefix(bare, "dressing:pendant:").size() == withPrefix(dressed, "dressing:pendant:").size());
    std::size_t hung = 0;
    for (const json& piece : withPrefix(bare, "dressing:pendant:")) {
        const std::string id = piece.at("id");
        const bool can = !id.starts_with("dressing:pendant:doorway/");
        if (id.ends_with(can ? "/stem" : "/canopy")) {
            INFO(id);
            CHECK(near(extentOf(piece).maxY, height));
            ++hung;
        }
    }
    CHECK(hung == 1 + (withPrefix(bare, "dressing:pendant:").size() - 5) / 3);
    CHECK(hasLight(bare, "look:pendant"));
}

TEST_CASE("world/room_dressing: the reader takes a pack's gallery, refusing a broken one (I12 step 2)",
          "[world][room_dressing]") {
    const auto parse = [](const std::string& dressing) {
        return rv::world::parseThemeMaterials(
            R"({"schemaVersion": 1, "id": "t", "name": "T", "heroParts": {}, "kinds": {}, "roomDressing": )" +
                dressing + "}",
            "t.json");
    };
    const auto refusal = [&](const std::string& dressing) {
        auto parsed = parse(dressing);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error().message;
    };
    auto good = parse(R"({"gallery": {"weights": {"poster": 4, "portrait": 2, "lightbox": 1, "frame": 3},
        "roles": ["side-module"], "frame": "MAT_Wood_Walnut", "picture": "MAT_Paper_Print", "lightbox": "MAT_Lightbox",
        "pattern": "print", "hood": "MAT_Metal_Brass", "wash": "MAT_Picture_Wash"}})");
    REQUIRE(good.has_value());
    REQUIRE(good->dressing.gallery.has_value());
    const rv::world::GalleryDressing& gallery = *good->dressing.gallery;
    CHECK(gallery.weights == std::array<int, 4>{4, 2, 1, 3});
    CHECK(gallery.roles == std::vector<FixtureRole>{FixtureRole::SideModule});
    CHECK(gallery.frame == "MAT_Wood_Walnut");
    CHECK(gallery.lightbox == "MAT_Lightbox");
    CHECK(gallery.hood == "MAT_Metal_Brass");
    CHECK(gallery.wash == "MAT_Picture_Wash");
    CHECK_FALSE(good->dressing.empty());
    // The modules by default; no picture light, no lightbox material without lightboxes.
    auto plain = parse(R"({"gallery": {"weights": {"poster": 1}, "frame": "MAT_Wood_Oak", "picture": "MAT_Paper_Print",
        "pattern": "print"}})");
    REQUIRE(plain.has_value());
    CHECK(plain->dressing.gallery->roles == std::vector<FixtureRole>{FixtureRole::HeroModule, FixtureRole::SideModule});
    CHECK(plain->dressing.gallery->hood.empty());
    CHECK(plain->dressing.gallery->lightbox.empty());

    const std::string base = R"("frame": "MAT_Wood_Oak", "picture": "MAT_Paper_Print", "pattern": "print")";
    CHECK_THAT(refusal(R"({"gallery": {"weights": {"poster": 0}, )" + base + "}}"), ContainsSubstring("weight"));
    CHECK_THAT(refusal(R"({"gallery": {"weights": {"lightbox": 1}, )" + base + "}}"), ContainsSubstring("lightbox"));
    CHECK_THAT(refusal(R"({"gallery": {"weights": {"mural": 1}, )" + base + "}}"), ContainsSubstring("mural"));
    CHECK_THAT(refusal(R"({"gallery": {"weights": {"poster": 1}, "hood": "MAT_Metal_Brass", )" + base + "}}"),
               ContainsSubstring("both or neither"));
    CHECK_THAT(refusal(R"({"gallery": {"weights": {"poster": 1}, "roles": ["attic"], )" + base + "}}"),
               ContainsSubstring("fixture role"));
    CHECK_THAT(refusal(R"({"gallery": {"weights": {"poster": 1}, "frame": "MAT_Wood_Oak", "picture": "MAT_Paper_Print",
        "pattern": "mona"}})"),
               ContainsSubstring("texture pattern"));
}

TEST_CASE("world/room_dressing: the gallery's frames over every golden plan room's modules, under the rail, every "
          "other one lit (I12 step 2)",
          "[world][room_dressing]") {
    std::set<rv::world::GalleryKind> kinds;
    for (const std::string& name : rv::test::world::goldenPlanNames()) {
        INFO(name);
        const rv::test::world::PlanCase& entry = rv::test::world::planCase(name);
        REQUIRE(entry.data.theme.dressing.gallery.has_value());
        const rv::world::GalleryDressing& gallery = *entry.data.theme.dressing.gallery;
        const rv::world::ShellDressing& shell = *entry.data.theme.dressing.shell;
        const json scene = sceneOf(entry.plan, entry.room, entry.data);
        CHECK(scene == sceneOf(entry.plan, entry.room, entry.data));
        const double bar = meters(rv::world::kGalleryFrameBarMm);
        const double rail = meters(shell.railYMm - 45);

        std::size_t modules = 0;
        std::size_t hung = 0;
        for (const rv::world::Fixture& fixture : entry.plan.fixtures) {
            if (fixture.role != FixtureRole::HeroModule && fixture.role != FixtureRole::SideModule) {
                continue;
            }
            ++modules;
            const std::string id = std::string(rv::world::kGalleryObjectPrefix) + fixture.id.localText();
            INFO(id);
            const json& frame = objectNamed(scene, id);
            CHECK(frame.at("prefab") == "poster_frame");
            CHECK(frame.at("collides") == false);
            CHECK(frame.at("castsShadows") == false);
            const double width = frame.at("params").at("width").get<double>();
            const double height = frame.at("params").at("height").get<double>();
            // Its kind from its picture's aspect: exactly that kind's picture.
            const rv::world::GalleryKind kind = rv::world::galleryKindOfAspect((width - 2 * bar) / (height - 2 * bar));
            kinds.insert(kind);
            const rv::world::GalleryPictureSize picture = rv::world::galleryPictureSize(kind);
            CHECK(near(width - 2 * bar, meters(picture.widthMm)));
            CHECK(near(height - 2 * bar, meters(picture.heightMm)));
            CHECK(width < meters(fixture.widthMm));
            CHECK(frame.at("materials").at("frame") == gallery.frame);
            CHECK(frame.at("materials").at("picture") ==
                  (kind == rv::world::GalleryKind::Lightbox ? gallery.lightbox : gallery.picture));
            CHECK(frame.at("textures").at("picture").at("pattern") == gallery.pattern);
            // Over its module, under the picture rail, turned as the module.
            const double centre = frame.at("position")[1].get<double>();
            CHECK(centre - height / 2 >= meters(fixture.yMm + fixture.heightMm) + 0.08 - 1e-6);
            CHECK(centre + height / 2 <= rail - 0.05 + 1e-6);
            CHECK(frame.at("rotationDeg")[1].get<double>() == static_cast<double>(fixture.yawCdeg) / 100.0);
            // Every other frame along the room a picture light (never over a lightbox): its hood over the frame, its
            // wash on the wall from the hood down to the frame's bottom.
            const bool lit = !gallery.hood.empty() && kind != rv::world::GalleryKind::Lightbox && hung % 2 == 0;
            CHECK(withPrefix(scene, id + "/").size() == (lit ? 3u : 0u));
            if (lit) {
                const Extent hood = extentOf(objectNamed(scene, id + "/hood"));
                CHECK(hood.minY > centre + height / 2);
                CHECK(hood.maxY < rail);
                const json& wash = objectNamed(scene, id + "/wash");
                CHECK(wash.at("material") == gallery.wash);
                const double washHeight = wash.at("params").at("size")[1].get<double>();
                CHECK(near(wash.at("position")[1].get<double>() - washHeight / 2, centre - height / 2));
                CHECK(wash.at("params").at("size")[0].get<double>() > width);
            }
            ++hung;
        }
        // One frame a module: nothing else under the gallery's prefix.
        std::size_t frames = 0;
        for (const json& object : withPrefix(scene, rv::world::kGalleryObjectPrefix)) {
            frames += object.contains("prefab") ? 1 : 0;
        }
        CHECK(frames == modules);
    }
    // The weights give every kind somewhere among the golden plans' rooms.
    CHECK(kinds.size() == 4);
}
