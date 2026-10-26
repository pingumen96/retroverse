// The theme packs per floor (I6 step 3, ADR-0014 §1, §5, docs/i6/look.md): every shipped pack read, complete and
// coherent; the pack a room is drawn with resolved by its floor, its kind (the arcade room) and its platform manifest;
// the pack's light (tints, the sign's neon); the update writing the resolved pack into the plan without moving
// anything; the label palettes the packs give the generated spines and labels.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <rv/assets/collection_labels.hpp>
#include <rv/assets/material_library.hpp>
#include <rv/core/math.hpp>
#include <rv/world/building.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/scene_builder.hpp>

#include "plan_scene_support.hpp"
#include "support/temp_dir.hpp"
#include "world_building_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::world::RoomKind;
using rv::world::SystemKind;

namespace {

const std::filesystem::path& assets() {
    return rv::test::world::sourceAssetRoot();
}

const rv::world::BuildingData& building() {
    static const rv::world::BuildingData data = *rv::world::loadBuildingData(assets());
    return data;
}

const rv::world::SystemsData& systems() {
    static const rv::world::SystemsData data = *rv::world::loadSystemsData(assets());
    return data;
}

const rv::assets::MaterialLibrary& library() {
    static const rv::assets::MaterialLibrary materials =
        *rv::assets::MaterialLibrary::loadFromFile(assets() / "materials" / "base" / "materials.json");
    return materials;
}

rv::assets::Rgb8 rgbOf(const std::string& hex) {
    const glm::vec4 colour = *rv::core::parseHexColor(hex);
    const auto byte = [](float value) { return static_cast<std::uint8_t>(std::lround(value * 255.0f)); };
    return {byte(colour.r), byte(colour.g), byte(colour.b)};
}

std::vector<rv::assets::Rgb8> paletteOf(const rv::world::ThemeMaterials& theme) {
    std::vector<rv::assets::Rgb8> palette;
    for (const std::string& hex : theme.labelPalette) {
        palette.push_back(rgbOf(hex));
    }
    return palette;
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

} // namespace

TEST_CASE("world/theme packs: the seven shipped packs, each complete and coherent", "[world][theme_packs]") {
    const std::vector<std::string> ids = rv::world::themePackIds(assets());
    CHECK(ids == std::vector<std::string>{"arcade_after_dark", "classic_16bit_warm", "computer_studio",
                                          "console_90s_collector", "handheld_archive", "optical_era_lounge",
                                          "retro_library_hub"});
    const rv::world::ThemeMaterials& builtIn = rv::world::builtInThemeMaterials();
    std::set<std::string> neons;
    for (const std::string& id : ids) {
        INFO(id);
        auto theme = rv::world::loadThemeMaterials(assets(), id);
        REQUIRE(theme.has_value());
        // Every kind and role the built-in pack dresses, the rooms' and the hall's, and no other kind.
        for (const auto& [kind, roles] : builtIn.kinds) {
            INFO(kind);
            REQUIRE(theme->kinds.contains(kind));
            for (const auto& [role, material] : roles) {
                INFO(role);
                CHECK(theme->kinds.at(kind).contains(role));
            }
            // Beyond them only the handheld archive's lit display cases (I6 step 6: "board_light").
            const bool litCases = id == "handheld_archive" && kind == "display_case";
            CHECK(theme->kinds.at(kind).size() == roles.size() + (litCases ? 1u : 0u));
        }
        CHECK(theme->kinds.size() == builtIn.kinds.size());
        // Every material is the library's.
        for (const auto* table : {&theme->kinds, &theme->heroParts}) {
            for (const auto& [name, roles] : *table) {
                for (const auto& [role, material] : roles) {
                    INFO(name << "." << role << " = " << material);
                    CHECK(library().find(material) != nullptr);
                }
            }
        }
        // The neon sign: lettering in the pack's neon colour, emissive enough to bloom.
        const std::map<std::string, std::string> sign = theme->materials("poster_frame", "sign");
        REQUIRE(sign.contains("picture"));
        const rv::assets::MaterialDesc* neon = library().find(sign.at("picture"));
        REQUIRE(neon != nullptr);
        CHECK(neon->shading == rv::assets::ShadingModel::Unlit);
        CHECK(neon->emissiveStrength >= 2.0f);
        const glm::vec3 expected = *rv::core::hexToLinearRgb(theme->neon);
        CHECK(std::abs(neon->baseColorFactor.r - expected.r) < 1e-4f);
        CHECK(std::abs(neon->baseColorFactor.g - expected.g) < 1e-4f);
        CHECK(std::abs(neon->baseColorFactor.b - expected.b) < 1e-4f);
        neons.insert(theme->neon);
        // The lit shelves: a strip and a glow, emissive (no light of their own).
        CHECK(theme->materials("shelf").at("board_light") == "MAT_Shelf_Light");
        CHECK(theme->materials("shelf").at("board_glow") == "MAT_Shelf_Glow");
        CHECK(theme->materials("wall_shelf").at("board_light") == "MAT_Shelf_Light");
        // A label palette of 8 colours, none twice; tints of known roles.
        CHECK(theme->labelPalette.size() == 8);
        CHECK(std::set<std::string>(theme->labelPalette.begin(), theme->labelPalette.end()).size() == 8);
    }
    // Each pack its own neon.
    CHECK(neons.size() == ids.size());
    // The built-in pack is classic_16bit_warm, and its palette assets::kDefaultLabelPalette (the SNES labels unchanged).
    auto classic = rv::world::loadThemeMaterials(assets(), "classic_16bit_warm");
    REQUIRE(classic.has_value());
    CHECK(*classic == builtIn);
    const std::vector<rv::assets::Rgb8> palette = paletteOf(*classic);
    CHECK(std::equal(palette.begin(), palette.end(), rv::assets::kDefaultLabelPalette.begin(),
                     rv::assets::kDefaultLabelPalette.end()));
    CHECK(classic->lightTints.empty());
}

TEST_CASE("world/theme packs: the reader refuses a broken palette, neon, glow, bloom, tint or lit shelf",
          "[world][theme_packs]") {
    const std::string head = R"({"schemaVersion": 1, "id": "t", "name": "T", "heroParts": {}, )";
    const auto parse = [&](const std::string& tail) {
        return rv::world::parseThemeMaterials(head + R"("kinds": {})" + tail + "}", "t.json");
    };
    const auto refusal = [&](const std::string& tail) {
        auto parsed = parse(tail);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error().message;
    };
    auto good = parse(R"(, "labelPalette": ["#000000", "#FFFFFF"], "neon": "#4C8BC9",
                         "neonGlow": {"intensity": 4.0, "range": 3.5}, "bloomStrength": 0.28,
                         "lightTints": {"picture": "#A0B0FF"})");
    REQUIRE(good.has_value());
    CHECK(good->neon == "#4C8BC9");
    CHECK(good->neonGlow == rv::world::NeonGlow{4.0, 3.5});
    CHECK(good->bloomStrength == 0.28);
    CHECK(good->lightTints.at("picture") == "#A0B0FF");
    // Every look key is optional: a pack without them takes classic_16bit_warm's palette and neon, the profile's glow
    // and bloom.
    auto bare = parse("");
    REQUIRE(bare.has_value());
    CHECK(bare->labelPalette.empty());
    CHECK(bare->neon == rv::world::builtInThemeMaterials().neon);
    CHECK_FALSE(bare->neonGlow.has_value());
    CHECK_FALSE(bare->bloomStrength.has_value());
    CHECK(bare->lightTints.empty());
    CHECK(bare->furniture.empty());
    CHECK_THAT(refusal(R"(, "labelPalette": ["#000000"])"), ContainsSubstring("2 to 16 colours"));
    CHECK_THAT(refusal(R"(, "labelPalette": ["#000000", "red"])"), ContainsSubstring("/labelPalette/1"));
    CHECK_THAT(refusal(R"(, "neon": "#4c8bc9")"), ContainsSubstring("/neon"));
    CHECK_THAT(refusal(R"(, "neonGlow": {"intensity": 4.0})"), ContainsSubstring("range"));
    CHECK_THAT(refusal(R"(, "neonGlow": {"intensity": -1, "range": 3})"), ContainsSubstring("/neonGlow/intensity"));
    CHECK_THAT(refusal(R"(, "bloomStrength": 1.5)"), ContainsSubstring("/bloomStrength"));
    CHECK_THAT(refusal(R"(, "lightTints": {"sun": "#FFFFFF"})"), ContainsSubstring("/lightTints/sun"));
    // Step 3's reserved decoration weights are gone: the weights are the furniture variants' (I6 step 3b).
    CHECK_THAT(refusal(R"(, "decorationWeights": {"plant": 0.5})"), ContainsSubstring("decorationWeights"));
    // A lit shelf names its strip and its glow together; a wall shelf has no glow.
    const auto kinds = [&](const std::string& table) {
        return rv::world::parseThemeMaterials(head + R"("kinds": )" + table + "}", "t.json");
    };
    CHECK(kinds(R"({"shelf": {"wood_primary": "MAT_A", "board_light": "MAT_L", "board_glow": "MAT_G"},
                    "wall_shelf": {"wood_primary": "MAT_A", "board_light": "MAT_L"}})")
              .has_value());
    CHECK_THAT(kinds(R"({"shelf": {"wood_primary": "MAT_A", "board_light": "MAT_L"}})").error().message,
               ContainsSubstring("board_light and board_glow together"));
    CHECK_THAT(kinds(R"({"shelf": {"wood_primary": "MAT_A", "board_glow": "MAT_G"}})").error().message,
               ContainsSubstring("/kinds/shelf"));
    CHECK_THAT(kinds(R"({"wall_shelf": {"wood_primary": "MAT_A", "board_light": "MAT_L", "board_glow": "MAT_G"}})")
                   .error()
                   .message,
               ContainsSubstring("/kinds/wall_shelf/board_glow"));
}

TEST_CASE("world/theme packs: a room's pack by its floor, the arcade room's by its kind", "[world][theme_packs]") {
    // building.json's floors (ADR-0014 §5).
    const std::vector<std::pair<std::string, std::string>> floors = {
        {"ground", "retro_library_hub"},   {"gen2", "classic_16bit_warm"},     {"gen3", "classic_16bit_warm"},
        {"gen4", "classic_16bit_warm"},    {"gen5", "console_90s_collector"},  {"gen6", "optical_era_lounge"},
        {"gen7", "optical_era_lounge"},    {"handheld", "handheld_archive"},   {"computer", "computer_studio"}};
    for (const auto& [floor, pack] : floors) {
        INFO(floor);
        CHECK(rv::world::floorThemePack(floor, systems(), building()) == pack);
        CHECK(rv::world::themePackOf(RoomKind::Console, floor, SystemKind::Home, systems(), building()) == pack);
        CHECK(rv::world::themePackOf(RoomKind::Shared, floor, SystemKind::Other, systems(), building()) == pack);
        // The arcade room wherever it stands.
        CHECK(rv::world::themePackOf(RoomKind::Arcade, floor, SystemKind::Arcade, systems(), building()) ==
              "arcade_after_dark");
    }
    // A floor naming no pack: its kind's defaults (systems.json).
    rv::world::BuildingData bare = building();
    for (rv::world::FloorSpec& spec : bare.floors) {
        spec.themePack.clear();
    }
    CHECK(rv::world::themePackOf(RoomKind::Console, "gen5", SystemKind::Home, systems(), bare) == "classic_16bit_warm");
    CHECK(rv::world::themePackOf(RoomKind::Console, "handheld", SystemKind::Handheld, systems(), bare) ==
          "handheld_archive");
    CHECK(rv::world::themePackOf(RoomKind::Shared, "computer", SystemKind::Computer, systems(), bare) ==
          "computer_studio");
    CHECK(rv::world::floorThemePack("ground", systems(), bare) == "arcade_after_dark");
    CHECK(rv::world::floorThemePack("no-such-floor", systems(), bare) == "computer_studio");
    // The floor's pack is a look: the plans' input digest of the building leaves it out.
    CHECK(rv::world::buildingDataToJson(bare) == rv::world::buildingDataToJson(building()));

    // A system's platform: by its floor, a stored room's recorded floor first; its manifest overrides.
    const auto platformOf = [&](const std::filesystem::path& root, std::string_view system, std::string_view floor) {
        auto platform = rv::world::resolvePlatform(root, rv::test::world::systemName(system), {}, systems(),
                                                   building(), floor);
        REQUIRE(platform.has_value());
        return platform->themePack;
    };
    // snes.json names no pack (ADR-0014 §1 as amended): its room takes its floor's, wherever it stands.
    CHECK(platformOf(assets(), "snes", {}) == "classic_16bit_warm");
    CHECK(platformOf(assets(), "snes", "gen5") == "console_90s_collector");
    CHECK(platformOf(assets(), "psx", {}) == "console_90s_collector");
    CHECK(platformOf(assets(), "ps2", {}) == "optical_era_lounge");
    CHECK(platformOf(assets(), "gb", {}) == "handheld_archive");
    CHECK(platformOf(assets(), "amiga", {}) == "computer_studio");
    CHECK(platformOf(assets(), "psx", "gen4") == "classic_16bit_warm"); // a room standing on another floor
    CHECK(rv::world::sharedRoomPlatform(RoomKind::Arcade, "ground", rv::test::world::systemName("mame"), systems(),
                                        building())
              .themePack == "arcade_after_dark");
    CHECK(rv::world::sharedRoomPlatform(RoomKind::Shared, "handheld", rv::test::world::systemName("gamecom"),
                                        systems(), building())
              .themePack == "handheld_archive");
    const rv::test::TempDir dir("rv-theme-packs");
    writeText(dir.path() / "world" / "platforms" / "psx.json",
              R"({"schemaVersion": 1, "system": "psx", "mediaFamily": "disc_case_cd", "themePack": "handheld_archive",
                  "plastic": {"cartridge": "#9A9A96", "console": "#D6CBAE", "accent": "#7D6E9E"}, "console": null,
                  "sign": {"text": "GREY DISCS"}})");
    // A manifest naming a pack overrides the floor's, on any floor.
    CHECK(platformOf(dir.path(), "psx", {}) == "handheld_archive");
    CHECK(platformOf(dir.path(), "psx", "gen4") == "handheld_archive");
    // One naming none keeps its floor's: the rest of the manifest is still its own.
    writeText(dir.path() / "world" / "platforms" / "psx.json",
              R"({"schemaVersion": 1, "system": "psx", "mediaFamily": "disc_case_cd",
                  "plastic": {"cartridge": "#9A9A96", "console": "#D6CBAE", "accent": "#7D6E9E"}, "console": null,
                  "sign": {"text": "GREY DISCS"}})");
    CHECK(platformOf(dir.path(), "psx", {}) == "console_90s_collector");
    CHECK(platformOf(dir.path(), "psx", "gen6") == "optical_era_lounge");
    auto unnamed = rv::world::resolvePlatform(dir.path(), rv::test::world::systemName("psx"), {}, systems(),
                                              building(), "gen6");
    REQUIRE(unnamed.has_value());
    CHECK(unnamed->signText == "GREY DISCS");
}

TEST_CASE("world/theme packs: the pack lights its rooms and halls", "[world][theme_packs]") {
    auto evening = rv::world::loadLightingProfile(assets(), "evening");
    auto afternoon = rv::world::loadLightingProfile(assets(), "afternoon");
    REQUIRE(evening.has_value());
    REQUIRE(afternoon.has_value());
    CHECK(rv::world::tintedColour("#FFC88A", "#FFFFFF") == "#FFC88A");
    CHECK(rv::world::tintedColour("#FFC88A", "#000000") == "#000000");
    CHECK(rv::world::tintedColour("#808080", "#808080") == "#3D3D3D");
    CHECK(rv::world::tintedColour("not a colour", "#808080") == "not a colour");
    // The sign's picture light from the neon: brought to full brightness, mixed with white.
    CHECK(rv::world::signLightTint("#B34DCE") == "#F0B7FF");
    CHECK(rv::world::signLightTint("#FFFFFF") == "#FFFFFF");
    CHECK(rv::world::signLightTint("#000000") == "#FFFFFF");
    CHECK(rv::world::signLightTint("magenta") == "#FFFFFF");

    for (const std::string& id : rv::world::themePackIds(assets())) {
        INFO(id);
        auto theme = rv::world::loadThemeMaterials(assets(), id);
        REQUIRE(theme.has_value());
        for (const rv::world::LightingProfile* profile : {&*evening, &*afternoon}) {
            const rv::world::LightingProfile lit = rv::world::themedLighting(*profile, *theme);
            // The picture light on the sign in the neon's hue, at the profile's brightness (no pack tints it by hand).
            CHECK_FALSE(theme->lightTints.contains("picture"));
            CHECK(lit.picture.color ==
                  rv::world::tintedColour(profile->picture.color, rv::world::signLightTint(theme->neon)));
            CHECK(lit.picture.intensity == profile->picture.intensity);
            // The neon's glow on the wall in its colour, the pack's intensity and range.
            for (const rv::world::AccentLight& accent : lit.accents) {
                INFO(accent.name);
                const auto before =
                    std::find_if(profile->accents.begin(), profile->accents.end(),
                                 [&](const rv::world::AccentLight& other) { return other.name == accent.name; });
                REQUIRE(before != profile->accents.end());
                if (accent.anchor == "sign") {
                    CHECK(accent.color == theme->neon);
                    CHECK(accent.intensity == (theme->neonGlow ? theme->neonGlow->intensity : before->intensity));
                    CHECK(accent.range == (theme->neonGlow ? theme->neonGlow->range : before->range));
                } else {
                    CHECK(accent == *before);
                }
            }
            CHECK(lit.bloomStrength == theme->bloomStrength.value_or(profile->bloomStrength));
            // Only colours, the glow and the bloom change: intensities, ranges and the environment as the profile's.
            CHECK(lit.key.intensity == profile->key.intensity);
            CHECK(lit.picture.range == profile->picture.range);
            CHECK(lit.roomLook() == profile->roomLook());
        }
    }
    // Classic's sign keeps today's magenta pool (#F0B8E8 before I6 step 3's review), its roles untinted.
    const rv::world::LightingProfile classic = rv::world::themedLighting(*evening, rv::world::builtInThemeMaterials());
    const glm::vec3 pool = *rv::core::hexToLinearRgb(classic.picture.color);
    CHECK(pool.r > pool.g);
    CHECK(pool.b > pool.g);
    CHECK(classic.key == evening->key);
    CHECK(classic.pendant == evening->pendant);
    CHECK(classic.wallWasher == evening->wallWasher);
    CHECK(classic.hall == evening->hall);

    auto optical = rv::world::loadThemeMaterials(assets(), "optical_era_lounge");
    REQUIRE(optical.has_value());
    const rv::world::LightingProfile lit = rv::world::themedLighting(*evening, *optical);
    // Its spots and fill cooler, its lamps warm for contrast, the sign's picture light blue.
    CHECK(lit.key.color != evening->key.color);
    CHECK(lit.pendant.color != evening->pendant.color);
    CHECK(lit.wallWasher.color != evening->wallWasher.color);
    CHECK(lit.lamp == evening->lamp);
    CHECK(lit.hall == evening->hall);
    const glm::vec3 picture = *rv::core::hexToLinearRgb(lit.picture.color);
    CHECK(picture.b > picture.r);
    // The arcade blooms more than every other pack.
    auto arcade = rv::world::loadThemeMaterials(assets(), "arcade_after_dark");
    REQUIRE(arcade.has_value());
    CHECK(rv::world::themedLighting(*evening, *arcade).bloomStrength > evening->bloomStrength);
}

TEST_CASE("world/theme packs: a room's scene data take the pack its floor resolves, never the plan's record",
          "[world][theme_packs]") {
    rv::test::world::BuildingWorld world = rv::test::world::shippedWorld(102938);
    const std::vector<rv::world::RoomSelection> selections = {
        rv::test::world::systemSelection("snes", 40, 11), rv::test::world::systemSelection("psx", 40, 12),
        rv::test::world::systemSelection("ps2", 40, 13), rv::test::world::systemSelection("gb", 40, 14),
        rv::test::world::systemSelection("amiga", 40, 15), rv::test::world::systemSelection("mame", 40, 16)};
    rv::world::GenerationResult generated = rv::test::world::generateBuildingOf(world, selections);
    const std::map<std::string, std::string> expected = {
        {"room:snes", "classic_16bit_warm"},      {"room:psx", "console_90s_collector"},
        {"room:ps2", "optical_era_lounge"},       {"room:gb", "handheld_archive"},
        {"room:amiga", "computer_studio"},        {"area:arcade", "arcade_after_dark"}};
    for (const auto& [id, pack] : expected) {
        INFO(id);
        const rv::world::Room* room = rv::test::world::findRoom(generated.plan, id);
        REQUIRE(room != nullptr);
        CHECK(room->themePack == pack);
    }
    // A plan recording other packs (written before I6 step 3) is drawn with the resolved ones.
    rv::world::WorldPlan stale = generated.plan;
    for (rv::world::Room& room : stale.rooms) {
        room.themePack = "classic_16bit_warm";
    }
    for (const auto& [id, pack] : expected) {
        INFO(id);
        const rv::world::Room* room = rv::test::world::findRoom(stale, id);
        auto data = rv::world::loadSceneData(assets(), stale, room->id);
        REQUIRE(data.has_value());
        CHECK(data->theme.id == pack);
        CHECK(data->platform.themePack == pack);
    }
    // The halls take their floor's pack.
    for (const auto& [floor, pack] : std::vector<std::pair<std::string, std::string>>{
             {"ground", "retro_library_hub"}, {"gen5", "console_90s_collector"}, {"handheld", "handheld_archive"}}) {
        INFO(floor);
        auto data = rv::world::loadFloorSceneData(assets(), stale, floor);
        REQUIRE(data.has_value());
        CHECK(data->hallTheme.id == pack);
    }
    // The arcade room blooms more than its floor's hall: its area carries its own bloom, the scene the hall's.
    auto ground = rv::world::loadFloorSceneData(assets(), stale, "ground");
    REQUIRE(ground.has_value());
    auto groundScene = rv::world::toFloorSceneFile(stale, "ground", *ground);
    REQUIRE(groundScene.has_value());
    const nlohmann::json& file = groundScene->sceneFile;
    CHECK(file["environment"]["bloomStrength"] == ground->lighting.bloomStrength);
    bool arcadeArea = false;
    for (const nlohmann::json& area : file["areas"]) {
        if (area["id"] == "area:arcade") {
            arcadeArea = true;
            CHECK(area["environment"]["bloomStrength"] == 0.28);
        } else {
            CHECK_FALSE((area.contains("environment") && area["environment"].contains("bloomStrength")));
        }
    }
    CHECK(arcadeArea);
}

TEST_CASE("world/theme packs: the cases and boxes on their side stand in shelves lit inside, under each room's pack",
          "[world][theme_packs]") {
    // I6 step 4's *_v2 families bring their own modules (furniture.shelf.*_module.*_side): the pack lights them like
    // every other shelf (step 3's board_light), the room's own pack's materials on them.
    rv::test::world::BuildingWorld world = rv::test::world::shippedWorld(102938);
    const std::vector<rv::world::RoomSelection> selections = {
        rv::test::world::systemSelection("psx", 40, 21), rv::test::world::systemSelection("ps2", 40, 22),
        rv::test::world::systemSelection("nds", 40, 23), rv::test::world::systemSelection("amiga", 40, 24),
        rv::test::world::systemSelection("pokemini", 10, 25)};
    const rv::world::GenerationResult generated = rv::test::world::generateBuildingOf(world, selections);
    const std::map<std::string, std::pair<std::string, std::string>> expected = {
        {"room:psx", {"disc_case_cd_v2", "console_90s_collector"}},
        {"room:ps2", {"disc_case_dvd_v2", "optical_era_lounge"}},
        {"room:nds", {"case_handheld_v2", "handheld_archive"}},
        {"room:amiga", {"big_box_v2", "computer_studio"}},
        {"area:shared:handheld", {"generic_case_v2", "handheld_archive"}}};
    for (const auto& [id, look] : expected) {
        INFO(id);
        const rv::world::Room* room = rv::test::world::findRoom(generated.plan, id);
        REQUIRE(room != nullptr);
        CHECK(room->mediaFamily == look.first);
        auto data = rv::world::loadSceneData(assets(), generated.plan, room->id);
        REQUIRE(data.has_value());
        CHECK(data->theme.id == look.second);
        auto build = rv::world::toSceneFile(generated.plan, room->id, *data);
        REQUIRE(build.has_value());
        std::map<std::string, const nlohmann::json*> objects;
        for (const nlohmann::json& object : build->sceneFile["objects"]) {
            objects[object["id"].get<std::string>()] = &object;
        }
        std::size_t sideModules = 0;
        for (const rv::world::Fixture& fixture : generated.plan.fixtures) {
            if (fixture.id.room() != room->id) {
                continue;
            }
            const rv::world::ScenePrefab* prefab = data->prefabs.find(fixture.prefab);
            REQUIRE(prefab != nullptr);
            if (prefab->generator != "shelf" && prefab->generator != "wall_shelf") {
                continue;
            }
            INFO(fixture.id.toString() << " " << fixture.prefab.manifest);
            const auto object = objects.find(fixture.id.localText());
            REQUIRE(object != objects.end());
            const nlohmann::json& params = (*object->second)["params"];
            CHECK(params.value("boardLight", false));
            const nlohmann::json& materials = (*object->second)["materials"];
            CHECK(materials["board_light"] == "MAT_Shelf_Light");
            CHECK(materials == nlohmann::json(data->theme.materials(prefab->generator)));
            if (fixture.prefab.manifest.find("_side.") != std::string::npos) {
                ++sideModules;
                CHECK(prefab->generator == "shelf");
                CHECK(params.value("back", false));
                CHECK(materials["board_glow"] == "MAT_Shelf_Glow");
            }
        }
        // Both hero-wall modules and the side walls' modules of the family on its side.
        CHECK(sideModules >= 2);
    }
}

TEST_CASE("world/theme packs: the update writes the resolved pack and moves nothing", "[world][theme_packs]") {
    rv::test::world::BuildingWorld world = rv::test::world::shippedWorld(102938);
    const std::vector<rv::world::RoomSelection> selections = {
        rv::test::world::systemSelection("snes", 40, 21), rv::test::world::systemSelection("psx", 40, 22),
        rv::test::world::systemSelection("gb", 40, 24), rv::test::world::systemSelection("mame", 40, 26)};
    rv::world::GenerationResult generated = rv::test::world::generateBuildingOf(world, selections);
    // A stored plan of I6 step 2: every room recorded with the one pack of the time.
    rv::world::WorldPlan stored = generated.plan;
    for (rv::world::Room& room : stored.rooms) {
        room.themePack = "classic_16bit_warm";
    }
    const rv::world::GenerationResult updated = rv::test::world::updateBuildingOf(world, stored, selections);
    CHECK(updated.changes.outcome == rv::world::GenerationOutcome::Updated);
    CHECK(updated.changes.keptRooms.empty());
    CHECK(updated.changes.added.empty());
    CHECK(updated.changes.kept == stored.placements.size());
    CHECK(updated.changes.fixturesAdded.empty());
    CHECK(updated.changes.fixturesRemoved.empty());
    CHECK(updated.changes.fixturesMoved.empty());
    CHECK(updated.changes.rebuild == rv::world::RebuildLevel::NoRebuild);
    CHECK(updated.validation.passed());
    // Nothing moved: every element as it stood, every room's record as it was but its pack, which is the resolved one.
    CHECK(updated.plan.placements == stored.placements);
    CHECK(updated.plan.fixtures == stored.fixtures);
    CHECK(updated.plan.lights == stored.lights);
    CHECK(updated.plan.dressing == stored.dressing);
    CHECK(updated.plan.unplaced == stored.unplaced);
    REQUIRE(updated.plan.rooms.size() == stored.rooms.size());
    for (std::size_t i = 0; i < stored.rooms.size(); ++i) {
        rv::world::Room room = updated.plan.rooms[i];
        INFO(room.id.toString());
        CHECK(room.themePack == rv::test::world::findRoom(generated.plan, room.id.toString())->themePack);
        room.themePack = stored.rooms[i].themePack;
        CHECK(room == stored.rooms[i]);
    }
    CHECK(updated.plan.rooms == generated.plan.rooms);
    CHECK(updated.changes.digestAfter != updated.changes.digestBefore);
    // Then an update with nothing new writes nothing.
    const rv::world::GenerationResult again = rv::test::world::updateBuildingOf(world, updated.plan, selections);
    CHECK(again.plan == updated.plan);
    CHECK(again.changes.digestAfter == again.changes.digestBefore);
}

TEST_CASE("world/theme packs: the update rewrites the pack of a kept, a held and an unselected room, nothing moved",
          "[world][theme_packs]") {
    rv::test::world::BuildingWorld world = rv::test::world::shippedWorld(102938);
    const std::vector<rv::world::RoomSelection> selections = {
        rv::test::world::systemSelection("snes", 40, 31), rv::test::world::systemSelection("psx", 40, 32),
        rv::test::world::systemSelection("gb", 30, 33), rv::test::world::systemSelection("pokemini", 8, 34),
        rv::test::world::systemSelection("gamegear", 8, 35)};
    const rv::world::GenerationResult generated = rv::test::world::generateBuildingOf(world, selections);
    const rv::world::Room* shared = rv::test::world::findRoom(generated.plan, "area:shared:handheld");
    REQUIRE(shared != nullptr);
    REQUIRE(shared->sections.size() == 2);
    // A stored plan of the old packs, its psx room of another generator major (kept as it is, ADR-0013 §5).
    rv::world::WorldPlan stored = generated.plan;
    for (rv::world::Room& room : stored.rooms) {
        room.themePack = "classic_16bit_warm";
        if (room.id.toString() == "room:psx") {
            room.generatorVersion = {0, 9};
        }
    }
    const auto packsResolved = [&](const rv::world::WorldPlan& plan) {
        REQUIRE(plan.rooms.size() == generated.plan.rooms.size());
        for (const rv::world::Room& room : plan.rooms) {
            INFO(room.id.toString());
            CHECK(room.themePack == rv::test::world::findRoom(generated.plan, room.id.toString())->themePack);
            CHECK(room.themePack == *rv::world::roomThemePack(assets(), room, {}, systems(), building()));
        }
    };
    const auto nothingMoved = [&](const rv::world::GenerationResult& updated) {
        CHECK(updated.plan.placements == stored.placements);
        CHECK(updated.plan.fixtures == stored.fixtures);
        CHECK(updated.plan.lights == stored.lights);
        CHECK(updated.plan.dressing == stored.dressing);
        CHECK(updated.changes.rebuild == rv::world::RebuildLevel::NoRebuild);
        CHECK(updated.validation.passed());
    };
    SECTION("kept and held rooms") {
        // Every selection but gamegear's: the shared room is held, room:psx kept, the others updated.
        const std::vector<rv::world::RoomSelection> some(selections.begin(), selections.end() - 1);
        const rv::world::GenerationResult updated = rv::test::world::updateBuildingOf(world, stored, some);
        CHECK(updated.changes.outcome == rv::world::GenerationOutcome::Updated);
        REQUIRE(updated.changes.keptRooms.size() == 1);
        CHECK(updated.changes.keptRooms.front().room.toString() == "room:psx");
        REQUIRE(updated.changes.heldRooms.size() == 1);
        CHECK(updated.changes.heldRooms.front().room.toString() == "area:shared:handheld");
        packsResolved(updated.plan);
        CHECK(rv::test::world::findRoom(updated.plan, "room:psx")->themePack == "console_90s_collector");
        CHECK(rv::test::world::findRoom(updated.plan, "area:shared:handheld")->themePack == "handheld_archive");
        nothingMoved(updated);
    }
    SECTION("a partial update") {
        // One room given its selection: every other room is left as it stands, its pack rewritten all the same; only
        // packs changed, so the plan keeps its provenance (which named more rooms).
        const rv::world::GenerationResult updated =
            rv::test::world::updateBuildingOf(world, stored, {selections.front()});
        CHECK(updated.changes.outcome == rv::world::GenerationOutcome::Updated);
        packsResolved(updated.plan);
        nothingMoved(updated);
        CHECK(updated.plan.inputDigest == stored.inputDigest);
        // Then the same update writes nothing.
        const rv::world::GenerationResult again =
            rv::test::world::updateBuildingOf(world, updated.plan, {selections.front()});
        CHECK(again.plan == updated.plan);
    }
}

TEST_CASE("world/theme packs: each pack's label palette colours its rooms' generated spines and labels",
          "[world][theme_packs]") {
    std::set<std::vector<std::uint8_t>> seen;
    for (const std::string& id : rv::world::themePackIds(assets())) {
        INFO(id);
        auto theme = rv::world::loadThemeMaterials(assets(), id);
        REQUIRE(theme.has_value());
        const std::vector<rv::assets::Rgb8> palette = paletteOf(*theme);
        // A spine without a cover takes the pack's colour for its seed; a label likewise (its cache key with it).
        std::set<std::size_t> used;
        for (std::uint64_t seed = 0; seed < 64; ++seed) {
            rv::assets::SpineRequest spine;
            spine.title = "A Title";
            spine.seed = seed;
            spine.palette = palette;
            const rv::assets::Rgb8 base = rv::assets::spineBaseColour(spine);
            const auto at = std::find(palette.begin(), palette.end(), base);
            REQUIRE(at != palette.end());
            used.insert(static_cast<std::size_t>(at - palette.begin()));
            CHECK(rv::assets::paletteColour(seed, palette) == base);
        }
        CHECK(used.size() == palette.size());
        std::vector<std::uint8_t> bytes;
        for (const rv::assets::Rgb8 colour : palette) {
            bytes.insert(bytes.end(), {colour.r, colour.g, colour.b});
        }
        if (id != "retro_library_hub") { // the hub keeps the classic palette
            CHECK(seen.insert(bytes).second);
        }
    }
    // No palette given: the default one, as before I6 step 3 (the SNES rooms' keys and pictures unchanged).
    rv::assets::SpineRequest spine;
    spine.seed = 7;
    CHECK(rv::assets::spineBaseColour(spine) == rv::assets::paletteColour(7));
    rv::assets::SpineRequest classic = spine;
    classic.palette = rv::assets::kDefaultLabelPalette;
    CHECK(rv::assets::spineBaseColour(classic) == rv::assets::spineBaseColour(spine));
}
