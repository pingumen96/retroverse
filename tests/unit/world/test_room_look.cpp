// The room's look as data (I3 step 16, rv/world/room_look.hpp): the lighting profiles under assets/world/lighting and
// the theme pack under assets/world/themes read and checked, world.json's choice of profile (never part of the input
// digest), the fallbacks, and the generated room's decoration that fits the side-wall sockets.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>

#include <rv/assets/material_library.hpp>
#include <rv/world/room_look.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_config.hpp>

#include "plan_scene_support.hpp"
#include "support/temp_dir.hpp"

using Catch::Matchers::ContainsSubstring;

namespace {

const std::filesystem::path& assets() {
    return rv::test::world::sourceAssetRoot();
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

} // namespace

TEST_CASE("world/room_look: the shipped lighting profiles read, two of them, the default evening", "[world][room_look]") {
    const std::vector<std::string> ids = rv::world::lightingProfileIds(assets());
    CHECK(ids == std::vector<std::string>{"afternoon", "evening"});
    CHECK(std::find(ids.begin(), ids.end(), std::string(rv::world::kDefaultLightingProfile)) != ids.end());
    auto library = rv::assets::MaterialLibrary::loadFromFile(assets() / "materials" / "base" / "materials.json");
    REQUIRE(library.has_value());
    for (const std::string& id : ids) {
        INFO(id);
        auto profile = rv::world::loadLightingProfile(assets(), id);
        REQUIRE(profile.has_value());
        CHECK(profile->name == id);
        // Warm light leads (style §4.1): the key and the washers are tungsten or neutral, never cold.
        for (const rv::world::LightStyle* style : {&profile->key, &profile->wallWasher, &profile->lamp}) {
            const auto rgb = *rv::core::hexToLinearRgb(style->color);
            CHECK(rgb.r >= rgb.b);
        }
        CHECK(profile->key.intensity > profile->wallWasher.intensity);
        // Accents on the hero corner's CRT and sign, and a museum spot on each display case and on the featured wall
        // shelf (style §4.1: accent lights on the cases).
        std::set<std::string> displayKinds;
        for (const rv::world::AccentLight& accent : profile->accents) {
            if (accent.on == rv::world::AccentAnchor::HeroCorner) {
                CHECK((accent.anchor == "crt" || accent.anchor == "sign"));
            } else {
                displayKinds.insert(accent.anchor);
                CHECK(accent.spot);
            }
        }
        CHECK(displayKinds == std::set<std::string>{"display_case", "wall_shelf"});
    }
    const auto evening = rv::world::loadLightingProfile(assets(), "evening");
    const auto afternoon = rv::world::loadLightingProfile(assets(), "afternoon");
    REQUIRE(evening.has_value());
    REQUIRE(afternoon.has_value());
    // The second profile is a different light, not a copy: brighter ambient, less bloom.
    CHECK(afternoon->ambientIntensity > evening->ambientIntensity);
    CHECK(afternoon->bloomStrength < evening->bloomStrength);
}

TEST_CASE("world/room_look: the profiles' hall role and environments light halls warmer and evener, never black",
          "[world][room_look]") {
    // I6 step 1 (ADR-0014 §4): the hall's lights, its doorway fill, its own environment, and the rooms' fill.
    for (const std::string id : {"evening", "afternoon"}) {
        INFO(id);
        const auto profile = rv::world::loadLightingProfile(assets(), id);
        REQUIRE(profile.has_value());
        const rv::world::HallLook& hall = profile->hall;
        // Warm light: never colder than neutral.
        for (const rv::world::LightStyle* style : {&hall.light, &hall.doorwayFill}) {
            const auto rgb = *rv::core::hexToLinearRgb(style->color);
            CHECK(rgb.r >= rgb.b);
            CHECK(style->intensity > 0.0);
        }
        // A low fill: dimmer than the hall's own lights, short enough to stay near its doorway.
        CHECK(hall.doorwayFill.intensity < hall.light.intensity);
        CHECK(hall.doorwayFill.range <= 8.0);
        // Upper walls and ceilings above black: some ambient from below (the floor's bounce) and an even fill.
        for (const rv::world::AreaLook& look : {profile->roomLook(), hall.environment, hall.lobbyEnvironment}) {
            const auto ground = *rv::core::hexToLinearRgb(look.ambientGround);
            CHECK(ground.r * look.ambientIntensity + look.fillIntensity > 0.05);
            CHECK(ground.r >= ground.b); // warm
            CHECK(look.exposure > 0.0);
        }
    }
    // The built-in profile lights rooms and halls as evening.json does (its roles are I3's test room's).
    const auto evening = rv::world::loadLightingProfile(assets(), "evening");
    REQUIRE(evening.has_value());
    const rv::world::LightingProfile& builtIn = rv::world::eveningLightingProfile();
    CHECK(builtIn.hall == evening->hall);
    // The lobby keeps step 1's ambient until the hub (I6 step 5), the halls a little dimmer; a profile without a
    // lobby of its own lights it as its halls.
    CHECK(evening->hall.lobbyEnvironment.ambientIntensity > evening->hall.environment.ambientIntensity);
    const auto afternoon = rv::world::loadLightingProfile(assets(), "afternoon");
    REQUIRE(afternoon.has_value());
    CHECK(afternoon->hall.lobbyEnvironment == afternoon->hall.environment);
    CHECK(builtIn.roomLook() == evening->roomLook());
    CHECK(builtIn.clearColor == evening->clearColor);

    // The hall role is required.
    auto text = rv::core::readTextFile(assets() / "world" / "lighting" / "evening.json");
    REQUIRE(text.has_value());
    std::string withoutHall = *text;
    const auto at = withoutHall.find("\"hall\": {");
    REQUIRE(at != std::string::npos);
    withoutHall.replace(at, 7, "\"halls\":"); // an unknown key, and the hall role missing
    CHECK_FALSE(rv::world::parseLightingProfile(withoutHall, "evening.json").has_value());
}

TEST_CASE("world/room_look: a broken profile is refused with its line, a missing one falls back", "[world][room_look]") {
    auto text = rv::core::readTextFile(assets() / "world" / "lighting" / "evening.json");
    REQUIRE(text.has_value());
    std::string broken = *text;
    const auto at = broken.find("\"intensity\": 70");
    REQUIRE(at != std::string::npos);
    broken.replace(at, 15, "\"intensity\": -1");
    auto refused = rv::world::parseLightingProfile(broken, "evening.json");
    REQUIRE_FALSE(refused.has_value());
    CHECK_THAT(refused.error().message, ContainsSubstring("/roles/key/intensity"));
    CHECK_THAT(refused.error().context, ContainsSubstring("evening.json:"));
    std::string unknown = *text;
    unknown.replace(unknown.find("\"keyShadowStrength\""), 19, "\"keyShadowStrengths\"");
    CHECK_FALSE(rv::world::parseLightingProfile(unknown, "evening.json").has_value());
    // An accent anchored on anything but a hero corner part or display fixtures.
    std::string anchoredElsewhere = *text;
    const auto on = anchoredElsewhere.find("\"on\": \"display\"");
    REQUIRE(on != std::string::npos);
    anchoredElsewhere.replace(on, 15, "\"on\": \"ceiling\"");
    auto badAnchor = rv::world::parseLightingProfile(anchoredElsewhere, "evening.json");
    REQUIRE_FALSE(badAnchor.has_value());
    CHECK_THAT(badAnchor.error().message, ContainsSubstring("expected hero-corner or display"));

    // world.json names a profile: it is used; one that does not exist falls back to the default.
    const rv::test::TempDir dir("rv-room-look");
    const std::filesystem::path config = dir.path() / "config";
    CHECK(rv::world::roomLightingProfile(assets(), config).name == "evening");
    writeText(config / "world.json", "{\"lightingProfile\": \"afternoon\"}\n");
    CHECK(rv::world::roomLightingProfile(assets(), config).name == "afternoon");
    writeText(config / "world.json", "{\"lightingProfile\": \"moonlight\"}\n");
    CHECK(rv::world::roomLightingProfile(assets(), config).name == "evening");
    // Without any profile file, the built-in one.
    CHECK(rv::world::roomLightingProfile(dir.path() / "no-assets", config) == rv::world::eveningLightingProfile());
}

TEST_CASE("world/room_look: the lighting profile is world.json's, outside the configuration that shapes the room",
          "[world][room_look]") {
    auto overrides = rv::world::parseWorldConfigOverrides("{\"lightingProfile\": \"afternoon\"}", "world.json");
    REQUIRE(overrides.has_value());
    REQUIRE(overrides->lightingProfile.has_value());
    auto config = rv::world::applyOverrides(rv::world::WorldConfig{}, *overrides);
    REQUIRE(config.has_value());
    CHECK(config->lightingProfile == "afternoon");
    CHECK(rv::world::WorldConfig{}.lightingProfile == rv::world::kDefaultLightingProfile);
    auto text = rv::world::writeWorldConfig(*config);
    REQUIRE(text.has_value());
    CHECK(*rv::world::readWorldConfig(*text, "config.json") == *config);
    CHECK_FALSE(rv::world::parseWorldConfigOverrides("{\"lightingProfile\": \"Not An Id\"}", "world.json").has_value());
}

TEST_CASE("world/room_look: the theme pack classic_16bit_warm names library materials, the built-in one its copy",
          "[world][room_look]") {
    auto theme = rv::world::loadThemeMaterials(assets(), "classic_16bit_warm");
    REQUIRE(theme.has_value());
    auto library = rv::assets::MaterialLibrary::loadFromFile(assets() / "materials" / "base" / "materials.json");
    REQUIRE(library.has_value());
    for (const auto* table : {&theme->kinds, &theme->heroParts}) {
        for (const auto& [name, roles] : *table) {
            for (const auto& [role, material] : roles) {
                INFO(name << "." << role << " = " << material);
                CHECK(library->find(material) != nullptr);
            }
        }
    }
    // The built-in copy is the file (the app's collection instance and the empty room's shell read it).
    CHECK(*theme == rv::world::builtInThemeMaterials());
    CHECK(theme->materials("cartridge") == rv::world::themeMaterials("cartridge"));
    CHECK(theme->materials("poster_frame", "sign").at("picture") == "MAT_Neon_Magenta");
    CHECK(theme->materials("poster_frame").at("picture") == "MAT_Paper_Print");
    CHECK_FALSE(rv::world::parseThemeMaterials(
                    "{\"schemaVersion\": 1, \"id\": \"t\", \"name\": \"T\", \"kinds\": {\"rug\": {\"fabric\": "
                    "\"Carpet\"}}, \"heroParts\": {}}",
                    "t.json")
                    .has_value());
}

TEST_CASE("world/room_look: every decoration of the template fits a side-wall socket", "[world][room_look]") {
    auto roomTemplate = rv::world::loadRoomTemplate(assets(), rv::world::kDefaultRoomTemplate);
    REQUIRE(roomTemplate.has_value());
    auto families = rv::assets::loadPrefabFamilies(assets());
    REQUIRE(families.has_value());
    const rv::world::TemplateSocket* side = nullptr;
    for (const rv::world::TemplateSocket& socket : roomTemplate->sockets) {
        if (socket.kind == rv::world::SocketKind::SideWall) {
            side = &socket;
            break;
        }
    }
    REQUIRE(side != nullptr);
    std::set<std::string> kinds;
    for (const rv::world::DecorationChoice& choice : roomTemplate->decoration) {
        INFO(choice.name);
        auto resolved = rv::assets::resolvePrefab(assets(), *families, choice.prefab.manifest);
        REQUIRE(resolved.has_value());
        CHECK(resolved->source == rv::assets::PrefabSource::Requested);
        REQUIRE(resolved->prefab.has_value());
        const rv::core::Aabb& bounds = resolved->prefab->bounds;
        CHECK(bounds.size().x * 1000.0f <= static_cast<float>(side->widthMm));
        CHECK(bounds.size().z * 1000.0f <= static_cast<float>(side->depthMm));
        kinds.insert(choice.prefab.kind);
    }
    // A poster, a lamp, a low cabinet and a plant (style §6.1's varied rhythm and heights).
    CHECK(kinds == std::set<std::string>{"floor_lamp", "plant", "poster_frame", "tv_cabinet"});
}
