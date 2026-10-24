// The theme packs per floor (I6 step 3, ADR-0014 §1, §5, docs/i6/look.md): every shipped pack read, complete and
// coherent; the pack a room is drawn with resolved by its floor, its kind (the arcade room) and its platform manifest;
// the pack's light (tints, the sign's neon); the update writing the resolved pack into the plan without moving
// anything; the label palettes the packs give the generated spines and labels.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <fstream>
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
            CHECK(theme->kinds.at(kind).size() == roles.size());
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
        // A label palette of 8 colours, none twice; tints of known roles; weights of the decoration's kinds.
        CHECK(theme->labelPalette.size() == 8);
        CHECK(std::set<std::string>(theme->labelPalette.begin(), theme->labelPalette.end()).size() == 8);
        CHECK(theme->decorationWeights.size() == 7);
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

TEST_CASE("world/theme packs: the reader refuses a broken palette, neon, tint or weight", "[world][theme_packs]") {
    const std::string head = R"({"schemaVersion": 1, "id": "t", "name": "T", "kinds": {}, "heroParts": {}, )";
    const auto parse = [&](const std::string& tail) { return rv::world::parseThemeMaterials(head + tail, "t.json"); };
    auto good = parse(R"("labelPalette": ["#000000", "#FFFFFF"], "neon": "#4C8BC9", "lightTints": {"picture": "#A0B0FF"},
                         "decorationWeights": {"plant": 0.5}})");
    REQUIRE(good.has_value());
    CHECK(good->neon == "#4C8BC9");
    CHECK(good->lightTints.at("picture") == "#A0B0FF");
    CHECK(good->decorationWeights.at("plant") == 0.5);
    CHECK_THAT(parse(R"("neon": "#4C8BC9"})").error().message, ContainsSubstring("labelPalette"));
    CHECK_THAT(parse(R"("labelPalette": ["#000000"], "neon": "#4C8BC9"})").error().message,
               ContainsSubstring("2 to 16 colours"));
    CHECK_THAT(parse(R"("labelPalette": ["#000000", "red"], "neon": "#4C8BC9"})").error().message,
               ContainsSubstring("/labelPalette/1"));
    CHECK_THAT(parse(R"("labelPalette": ["#000000", "#FFFFFF"]})").error().message, ContainsSubstring("neon"));
    CHECK_THAT(parse(R"("labelPalette": ["#000000", "#FFFFFF"], "neon": "#4C8BC9", "lightTints": {"sun": "#FFFFFF"}})")
                   .error()
                   .message,
               ContainsSubstring("/lightTints/sun"));
    CHECK_THAT(
        parse(R"("labelPalette": ["#000000", "#FFFFFF"], "neon": "#4C8BC9", "decorationWeights": {"plant": 2}})")
            .error()
            .message,
        ContainsSubstring("from 0 to 1"));
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
    CHECK(platformOf(assets(), "snes", {}) == "classic_16bit_warm"); // snes.json names it
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
    CHECK(platformOf(dir.path(), "psx", {}) == "handheld_archive");
}

TEST_CASE("world/theme packs: the pack lights its rooms and halls", "[world][theme_packs]") {
    auto evening = rv::world::loadLightingProfile(assets(), "evening");
    auto afternoon = rv::world::loadLightingProfile(assets(), "afternoon");
    REQUIRE(evening.has_value());
    REQUIRE(afternoon.has_value());
    // The classic pack leaves the profiles as they are (the SNES rooms' light unchanged).
    CHECK(rv::world::themedLighting(*evening, rv::world::builtInThemeMaterials()) == *evening);
    CHECK(rv::world::themedLighting(*afternoon, rv::world::builtInThemeMaterials()) == *afternoon);
    CHECK(rv::world::tintedColour("#FFC88A", "#FFFFFF") == "#FFC88A");
    CHECK(rv::world::tintedColour("#FFC88A", "#000000") == "#000000");
    CHECK(rv::world::tintedColour("#808080", "#808080") == "#3D3D3D");
    CHECK(rv::world::tintedColour("not a colour", "#808080") == "not a colour");

    auto optical = rv::world::loadThemeMaterials(assets(), "optical_era_lounge");
    REQUIRE(optical.has_value());
    const rv::world::LightingProfile lit = rv::world::themedLighting(*evening, *optical);
    // Its spots a little cooler, its lamps warm for contrast, the sign's picture light and glow blue.
    CHECK(lit.key.color != evening->key.color);
    CHECK(lit.wallWasher.color != evening->wallWasher.color);
    CHECK(lit.lamp == evening->lamp);
    CHECK(lit.pendant == evening->pendant);
    CHECK(lit.hall == evening->hall);
    const glm::vec3 picture = *rv::core::hexToLinearRgb(lit.picture.color);
    CHECK(picture.b > picture.r);
    for (const rv::world::AccentLight& accent : lit.accents) {
        INFO(accent.name);
        CHECK(accent.color == (accent.anchor == "sign" ? optical->neon : [&] {
                  for (const rv::world::AccentLight& before : evening->accents) {
                      if (before.name == accent.name) {
                          return before.color;
                      }
                  }
                  return std::string();
              }()));
        CHECK(accent.intensity > 0.0);
    }
    // Only colours change: intensities, ranges and cones as the profile's.
    CHECK(lit.key.intensity == evening->key.intensity);
    CHECK(lit.picture.range == evening->picture.range);
    CHECK(lit.roomLook() == evening->roomLook());
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
    classic.palette.assign(rv::assets::kDefaultLabelPalette.begin(), rv::assets::kDefaultLabelPalette.end());
    CHECK(rv::assets::spineBaseColour(classic) == rv::assets::spineBaseColour(spine));
}
