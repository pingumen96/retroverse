// The world's data files under assets/world (ADR-0010 §2, §4) and their readers (rv/world/room_template.hpp,
// platform_manifest.hpp, media_family.hpp): every shipped file is read, holds what the ADR decided (the template's 16
// collection sockets, its 2 door-reserved and 3 display sockets, its socket order and growth), names prefabs by
// manifest id only (no model path anywhere), and a malformed file is refused with the line of the problem.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/world/media_family.hpp>
#include <rv/world/platform_manifest.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_config.hpp>

#include "support/temp_dir.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using rv::world::RoomTemplate;
using rv::world::SocketKind;
using rv::world::TemplateSocket;
using rv::world::Wall;

namespace {

const std::filesystem::path kAssets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
const std::filesystem::path kTemplatePath = kAssets / "world" / "templates" / "console_room_9x10.json";

std::string readText(const std::filesystem::path& path) {
    auto text = rv::core::readTextFile(path);
    REQUIRE(text.has_value());
    return *text;
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

std::size_t lineOf(std::string_view text, std::string_view needle) {
    const std::size_t at = text.find(needle);
    REQUIRE(at != std::string_view::npos);
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
}

RoomTemplate shippedTemplate() {
    auto roomTemplate = rv::world::loadRoomTemplate(kAssets, "console_room_9x10");
    REQUIRE(roomTemplate.has_value());
    return *roomTemplate;
}

std::vector<const TemplateSocket*> socketsOf(const RoomTemplate& roomTemplate, SocketKind kind,
                                             std::uint32_t maxGrowthStep = 0) {
    std::vector<const TemplateSocket*> sockets;
    for (const TemplateSocket& socket : roomTemplate.sockets) {
        if (socket.kind == kind && socket.growthStep <= maxGrowthStep) {
            sockets.push_back(&socket);
        }
    }
    return sockets;
}

/// Every prefab the world's data names.
std::set<rv::world::PrefabRef> namedPrefabs() {
    std::set<rv::world::PrefabRef> prefabs;
    const RoomTemplate roomTemplate = shippedTemplate();
    prefabs.insert(roomTemplate.heroModule);
    prefabs.insert(roomTemplate.sideModule);
    for (const TemplateSocket& socket : roomTemplate.sockets) {
        if (socket.prefab) {
            prefabs.insert(*socket.prefab);
        }
        for (const rv::world::HeroPart& part : socket.parts) {
            if (part.prefab) {
                prefabs.insert(*part.prefab);
            }
        }
    }
    for (const rv::world::DecorationChoice& choice : roomTemplate.decoration) {
        prefabs.insert(choice.prefab);
    }
    prefabs.insert(roomTemplate.shelfDressing.begin(), roomTemplate.shelfDressing.end());
    auto platform = rv::world::loadPlatformManifest(kAssets, *rv::catalog::SystemName::parse("snes"));
    REQUIRE(platform.has_value());
    prefabs.insert(platform->console);
    auto families = rv::world::loadMediaFamilies(kAssets);
    REQUIRE(families.has_value());
    for (const rv::world::MediaFamily& family : families->families) {
        prefabs.insert(family.item);
    }
    return prefabs;
}

rv::Error templateRefusal(const std::string& text) {
    auto roomTemplate = rv::world::parseRoomTemplate(text, "console_room_9x10.json");
    REQUIRE_FALSE(roomTemplate.has_value());
    return roomTemplate.error();
}

} // namespace

TEST_CASE("world/data: every file under assets/world is read", "[world][data]") {
    std::size_t read = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(kAssets / "world")) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), kAssets / "world");
        const std::string folder = relative.has_parent_path() ? relative.parent_path().generic_string() : "";
        const std::string stem = entry.path().stem().string();
        INFO(relative.generic_string());
        REQUIRE(entry.path().extension() == ".json");
        if (folder == "templates") {
            CHECK(rv::world::loadRoomTemplate(kAssets, stem).has_value());
        } else if (folder == "platforms") {
            CHECK(rv::world::loadPlatformManifest(kAssets, *rv::catalog::SystemName::parse(stem)).has_value());
        } else if (folder == "presets") {
            CHECK(rv::world::loadPreset(kAssets, stem).has_value());
        } else {
            REQUIRE(relative.generic_string() == "media_families.json");
            CHECK(rv::world::loadMediaFamilies(kAssets).has_value());
        }
        ++read;
    }
    CHECK(read == 4);
}

TEST_CASE("world/data: no model path anywhere in assets/world", "[world][data]") {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(kAssets / "world")) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string text = readText(entry.path());
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        INFO(entry.path().generic_string());
        CHECK(text.find(".glb") == std::string::npos);
        CHECK(text.find(".gltf") == std::string::npos);
    }
}

TEST_CASE("world/data: the console room template holds ADR-0010's sockets", "[world][data]") {
    const RoomTemplate room = shippedTemplate();
    CHECK(room.id == "console_room_9x10");
    CHECK(room.version == 1);
    CHECK(room.widthMm == 9000);
    CHECK(room.depthMm == 10000);
    CHECK(room.heightMm == 3200);
    CHECK(room.doorway.wall == Wall::South);
    CHECK(room.doorway.widthMm == 1600);
    CHECK(room.doorway.clearWidthMm == 1600);
    CHECK(room.doorway.clearDepthMm == 1500);
    CHECK(room.circulationWidthMm == 1400);
    CHECK(room.eyeHeightMm == 1300);
    CHECK(room.lightLimit == 16);

    // 16 collection sockets in the base room: two hero-wall modules and seven side-wall sockets on each side wall.
    const auto heroModules = socketsOf(room, SocketKind::HeroModule);
    const auto sideWall = socketsOf(room, SocketKind::SideWall);
    CHECK(heroModules.size() == 2);
    CHECK(sideWall.size() == 14);
    CHECK(std::count_if(sideWall.begin(), sideWall.end(),
                        [](const TemplateSocket* s) { return s->wall == Wall::West; }) == 7);
    for (const TemplateSocket* socket : sideWall) {
        CHECK(socket->widthMm == 900);
    }
    // Two door-reserved sockets of 1.8 m, one in each side wall.
    const auto doors = socketsOf(room, SocketKind::DoorReserved);
    REQUIRE(doors.size() == 2);
    CHECK(((doors[0]->wall == Wall::West && doors[1]->wall == Wall::East) ||
           (doors[0]->wall == Wall::East && doors[1]->wall == Wall::West)));
    CHECK(doors[0]->widthMm == 1800);
    CHECK(doors[1]->widthMm == 1800);
    // The display sockets: the featured wall shelf beside the hero corner, then the two display cases.
    const auto displays = socketsOf(room, SocketKind::Display);
    CHECK(displays.size() == 3);
    CHECK(room.displayOrder == std::vector<std::string>{"fs01", "dc01", "dc02"});
    CHECK(room.socket("fs01")->wall == Wall::North);
    CHECK(room.socket("dc01")->wall == Wall::None);
    // The hero band across the two hero-wall modules, the west one first.
    CHECK(room.heroBand == std::vector<std::string>{"hw-w", "hw-e"});
    CHECK(room.socket("hw-w")->xMm < room.socket("hw-e")->xMm);
    // The hero corner on the hero wall, with the TV and the console.
    const auto corners = socketsOf(room, SocketKind::HeroCorner);
    REQUIRE(corners.size() == 1);
    CHECK(corners[0]->wall == Wall::North);
    CHECK(std::any_of(corners[0]->parts.begin(), corners[0]->parts.end(),
                      [](const rv::world::HeroPart& part) { return part.role == "crt" && part.prefab; }));
    CHECK(std::any_of(corners[0]->parts.begin(), corners[0]->parts.end(),
                      [](const rv::world::HeroPart& part) { return part.role == "console" && !part.prefab; }));
}

TEST_CASE("world/data: the socket order frames the hero wall and grows towards the doorway", "[world][data]") {
    const RoomTemplate room = shippedTemplate();
    REQUIRE(room.collectionOrder.size() == 28);
    CHECK(room.collectionOrder[0] == "hw-w");
    CHECK(room.collectionOrder[1] == "hw-e");
    // Then the side-wall sockets from north to south, alternating west and east.
    std::int64_t previousZ = -100'000;
    for (std::size_t i = 2; i < room.collectionOrder.size(); ++i) {
        const TemplateSocket* socket = room.socket(room.collectionOrder[i]);
        REQUIRE(socket != nullptr);
        INFO(socket->id);
        CHECK(socket->kind == SocketKind::SideWall);
        CHECK(socket->wall == (i % 2 == 0 ? Wall::West : Wall::East));
        if (i % 2 == 0) {
            CHECK(socket->zMm > previousZ);
            previousZ = socket->zMm;
        } else {
            CHECK(socket->zMm == previousZ);
        }
    }
    std::vector<std::string> first;
    std::copy_n(room.collectionOrder.begin(), 6, std::back_inserter(first));
    CHECK(first == std::vector<std::string>{"hw-w", "hw-e", "w01", "e01", "w02", "e02"});
}

TEST_CASE("world/data: the room grows south in 2 m steps to 16 m", "[world][data]") {
    const RoomTemplate room = shippedTemplate();
    CHECK(room.growthStepMm == 2000);
    CHECK(room.maxDepthMm == 16000);
    REQUIRE(room.maxGrowthSteps() == 3);
    // Each step adds two side-wall sockets to each side wall: 26 at 16 m (ADR-0010 §5).
    for (std::uint32_t steps = 0; steps <= 3; ++steps) {
        CHECK(socketsOf(room, SocketKind::SideWall, steps).size() == 14 + 4 * steps);
    }
    const rv::world::Footprint base = rv::world::interiorFootprint(room, 0);
    CHECK(base == rv::world::Footprint{-4500, -5000, 4500, 5000});
    const rv::world::Footprint grown = rv::world::interiorFootprint(room, 3);
    CHECK(grown == rv::world::Footprint{-4500, -5000, 4500, 11000});
    // The south wall's sockets and the doorway move with it; the rest stays.
    const TemplateSocket& southDecoration = *room.socket("dec-sw");
    CHECK(rv::world::socketFootprint(room, southDecoration, 3).maxZMm ==
          rv::world::socketFootprint(room, southDecoration, 0).maxZMm + 6000);
    CHECK(rv::world::doorwayClearance(room, 0) == rv::world::Footprint{-800, 3500, 800, 5000});
    CHECK(rv::world::doorwayClearance(room, 3) == rv::world::Footprint{-800, 9500, 800, 11000});
    const TemplateSocket& w01 = *room.socket("w01");
    CHECK(rv::world::socketFootprint(room, w01, 3) == rv::world::socketFootprint(room, w01, 0));
    CHECK(rv::world::socketFootprint(room, w01, 0) == rv::world::Footprint{-4470, -4450, -4070, -3550});
    // A display case facing east from the circulation band's edge.
    CHECK(rv::world::socketFootprint(room, *room.socket("dc01"), 0) == rv::world::Footprint{-1250, 400, -750, 1600});
}

TEST_CASE("world/data: the platform manifest and the media family", "[world][data]") {
    auto platform = rv::world::loadPlatformManifest(kAssets, *rv::catalog::SystemName::parse("snes"));
    REQUIRE(platform.has_value());
    CHECK(platform->system.str() == "snes");
    CHECK(platform->mediaFamily == "cartridge_home_16bit");
    CHECK(platform->themePack == "classic_16bit_warm");
    CHECK(platform->console == rv::world::PrefabRef{"hardware.console.cartridge.generic", "console_cartridge"});
    CHECK(platform->signText == "16-BIT CORNER");
    CHECK(rv::world::isSrgbHexColor(platform->cartridgePlastic));

    auto families = rv::world::loadMediaFamilies(kAssets);
    REQUIRE(families.has_value());
    const rv::world::MediaFamily* family = families->find(platform->mediaFamily);
    REQUIRE(family != nullptr);
    CHECK(families->find("dvd_case") == nullptr);
    // 12.5 x 8.5 x 2.0 cm; spine-out a 2.0 x 12.5 cm spine 8.5 cm deep at a 2.4 cm pitch.
    CHECK(family->itemWidthMm == 125);
    CHECK(family->itemHeightMm == 85);
    CHECK(family->itemDepthMm == 20);
    CHECK(family->spineOut.widthMm == 20);
    CHECK(family->spineOut.heightMm == 125);
    CHECK(family->spineOut.depthMm == 85);
    CHECK(family->spineOut.widthMm + family->spineOut.gapMm == 24);
    CHECK(family->faceOut.widthMm == 125);

    // The family's item agrees with its manifest.
    auto manifest =
        rv::assets::loadPrefabManifest(kAssets / "manifests" / "prefabs" / (family->item.manifest + ".json"));
    REQUIRE(manifest.has_value());
    CHECK(rv::world::checkItemManifest(*family, *manifest).has_value());
    rv::assets::PrefabManifest resized = *manifest;
    resized.runtime.scaleMeters = glm::vec3(0.126f, 0.085f, 0.02f);
    auto mismatch = rv::world::checkItemManifest(*family, resized);
    REQUIRE_FALSE(mismatch.has_value());
    CHECK_THAT(mismatch.error().message, ContainsSubstring("126 x 85 x 20"));
    rv::assets::PrefabManifest other = *manifest;
    other.runtime.generator = "crt";
    CHECK_FALSE(rv::world::checkItemManifest(*family, other).has_value());
}

TEST_CASE("world/data: every prefab the data names has a manifest, or is one a later step writes", "[world][data]") {
    // The manifests of I3 step 1b (the modules, the display fixtures, the shelf dressing) and the hero corner's
    // armchair, which no manifest names yet (docs/i3/step-1a.md, "Notes for later steps").
    const std::set<std::string> pending = {"furniture.shelf.hero_module.generic",
                                           "furniture.shelf.side_module.generic",
                                           "furniture.wall_shelf.featured.generic",
                                           "furniture.display_case.generic",
                                           "decor.storage_box.generic",
                                           "decor.magazine_stack.generic",
                                           "decor.bookends.generic",
                                           "furniture.armchair.generic"};
    for (const rv::world::PrefabRef& prefab : namedPrefabs()) {
        INFO(prefab.manifest);
        CHECK(rv::assets::isValidPrefabId(prefab.manifest));
        const std::filesystem::path path = kAssets / "manifests" / "prefabs" / (prefab.manifest + ".json");
        if (!std::filesystem::exists(path)) {
            CHECK(pending.contains(prefab.manifest));
            continue;
        }
        auto manifest = rv::assets::loadPrefabManifest(path);
        REQUIRE(manifest.has_value());
        CHECK(manifest->runtime.generator.value_or("") == prefab.kind);
    }
}

TEST_CASE("world/data: a malformed template is refused with its line", "[world][data]") {
    const std::string shipped = readText(kTemplatePath);
    struct Case {
        std::string_view from;
        std::string_view to;
        std::string_view message;
        std::string_view lineOf; ///< text on the line the error must name (in the edited text)
    };
    const Case cases[] = {
        {"\"widthMm\": 900, \"depthMm\": 400},\n    // The hero corner",
         "\"widthMm\": 900.5, \"depthMm\": 400},\n    // The hero corner",
         "/sockets/0/widthMm: expected an integer, found a number with a fraction", "900.5"},
        {"\"id\": \"w02\", \"kind\": \"side-wall\"", "\"id\": \"w02\", \"kind\": \"side-wall\", \"colour\": 1",
         "/sockets/7/colour: unknown key", "\"id\": \"w02\""},
        {"\"id\": \"w02\", \"kind\": \"side-wall\"", "\"id\": \"w02\", \"kind\": \"shelf\"",
         "/sockets/7/kind: expected one of", "\"id\": \"w02\""},
        {"\"id\": \"w02\"", "\"id\": \"w01\"", "/sockets/7/id: the socket appears twice",
         "\"id\": \"w01\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": -3000"},
        {"\"id\": \"w01\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": -4000, \"yawCdeg\": "
         "9000",
         "\"id\": \"w01\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": -4000, \"yawCdeg\": "
         "27000",
         "faces into the room: yaw 9000", "\"id\": \"w01\""},
        {"\"id\": \"w01\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470",
         "\"id\": \"w01\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4300",
         "stands 200 mm from the west wall", "\"id\": \"w01\""},
        {"\"id\": \"dec-nw\", \"kind\": \"decoration\", \"wall\": \"north\", \"xMm\": -3650",
         "\"id\": \"dec-nw\", \"kind\": \"decoration\", \"wall\": \"north\", \"xMm\": -2500",
         "the socket overlaps \"hw-w\" after 0 growth step(s)", "\"id\": \"dec-nw\""},
        {"\"id\": \"dec-sw\", \"kind\": \"decoration\", \"wall\": \"south\", \"xMm\": -2500",
         "\"id\": \"dec-sw\", \"kind\": \"decoration\", \"wall\": \"south\", \"xMm\": -1200",
         "enters the doorway's clearance after 0 growth step(s)", "\"id\": \"dec-sw\""},
        {"\"id\": \"w08\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": 5000",
         "\"id\": \"w08\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": 5600",
         "the socket overlaps \"w08\" after 1 growth step(s)", "\"id\": \"w09\""},
        {"\"id\": \"w09\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": 6000",
         "\"id\": \"w09\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": 6600",
         "leaves the room", "\"id\": \"w09\""},
        {"\"widthMm\": 900, \"depthMm\": 400, \"growthStep\": 3}\n  ],",
         "\"widthMm\": 900, \"depthMm\": 400, \"growthStep\": 4}\n  ],", "the room takes at most 3 growth steps",
         "\"growthStep\": 4"},
        {"\"id\": \"door-w\", \"kind\": \"door-reserved\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": 500, "
         "\"yawCdeg\": 9000,\n     \"widthMm\": 1800, \"depthMm\": 400}",
         "\"id\": \"door-w\", \"kind\": \"door-reserved\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": 500, "
         "\"yawCdeg\": 9000,\n     \"widthMm\": 1800, \"depthMm\": 400, "
         "\"prefab\": {\"manifest\": \"decor.door_mat.generic\", \"kind\": \"rug\"}}",
         "/sockets/10/prefab: only a display socket names its prefab", "door_mat"},
        {"{\"role\": \"console\", \"prefab\": null",
         "{\"role\": \"console\", \"prefab\": {\"manifest\": \"a.b\", \"kind\": \"c\"}",
         "/sockets/1/parts/2/prefab: the console is the platform manifest's: null", "\"role\": \"console\""},
        {"{\"id\": \"w07\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": 4000, "
         "\"yawCdeg\": 9000,\n     \"widthMm\": 900, \"depthMm\": 400}",
         "{\"id\": \"w07\", \"kind\": \"side-wall\", \"wall\": \"west\", \"xMm\": -4470, \"zMm\": 4000, "
         "\"yawCdeg\": 9000,\n     \"widthMm\": 900, \"depthMm\": 400, \"growthStep\": 1}",
         "/collectionOrder/15: a growth step's sockets come after", "\"w07\", \"e07\""},
        {"\"w13\", \"e13\"\n", "\"w13\"\n", "/collectionOrder: does not name the socket \"e13\"", "collectionOrder"},
        {"\"heroBand\": [\"hw-w\", \"hw-e\"]", "\"heroBand\": [\"hw-w\", \"w01\"]",
         "\"w01\" is a side-wall socket, not a hero-module one", "\"heroBand\""},
        {"\"displayOrder\": [\"fs01\", \"dc01\", \"dc02\"]", "\"displayOrder\": [\"fs01\", \"dc01\", \"dc01\"]",
         "names \"dc01\" twice", "\"displayOrder\""},
        {"\"maxDepthMm\": 16000", "\"maxDepthMm\": 15000", "/growth/maxDepthMm: the room's depth plus a whole number",
         "\"maxDepthMm\": 15000"},
        {"\"lightLimit\": 16", "\"lightLimit\": 17", "/lightLimit: expected an integer from 1 to 16", "lightLimit"},
        {"\"manifest\": \"decor.bookends.generic\"", "\"manifest\": \"decor/bookends.glb\"",
         "/shelfDressing/2/manifest: expected a prefab manifest id", "bookends.glb"},
        {"\"id\": \"dec-ne\", \"kind\": \"decoration\"", "\"id\": \"dec-ne\", \"kind\": \"hero-corner\"",
         "/sockets/5/parts: missing", "\"id\": \"dec-ne\""},
    };
    for (const Case& test : cases) {
        INFO(test.to);
        const std::string text = replaced(shipped, test.from, test.to);
        const rv::Error error = templateRefusal(text);
        CHECK(error.code == rv::ErrorCode::ValidationFailed);
        CHECK_THAT(error.message, ContainsSubstring(std::string(test.message)));
        CHECK(error.context == "console_room_9x10.json:" + std::to_string(lineOf(text, test.lineOf)));
    }

    // A syntax error and a key twice name their line too.
    const std::string missingComma = replaced(shipped, "\"version\": 1,", "\"version\": 1");
    const rv::Error syntax = templateRefusal(missingComma);
    CHECK(syntax.code == rv::ErrorCode::ParseError);
    CHECK(syntax.context == "console_room_9x10.json:" + std::to_string(lineOf(missingComma, "\"room\":")));
    const std::string twice =
        replaced(shipped, "\"eyeHeightMm\": 1300,", "\"eyeHeightMm\": 1300,\n  \"eyeHeightMm\": 1200,");
    const rv::Error duplicate = templateRefusal(twice);
    CHECK_THAT(duplicate.message, ContainsSubstring("/eyeHeightMm: the key appears twice"));
    CHECK(duplicate.context == "console_room_9x10.json:" + std::to_string(lineOf(twice, "\"eyeHeightMm\": 1200")));
}

TEST_CASE("world/data: a file's id must be its name", "[world][data]") {
    const rv::test::TempDir dir;
    const std::filesystem::path root = dir.path() / "assets";
    std::filesystem::create_directories(root / "world" / "templates");
    std::filesystem::create_directories(root / "world" / "platforms");
    std::filesystem::copy_file(kTemplatePath, root / "world" / "templates" / "lounge.json");
    std::filesystem::copy_file(kAssets / "world" / "platforms" / "snes.json",
                               root / "world" / "platforms" / "nes.json");

    auto roomTemplate = rv::world::loadRoomTemplate(root, "lounge");
    REQUIRE_FALSE(roomTemplate.has_value());
    CHECK_THAT(roomTemplate.error().message, ContainsSubstring("is not its file's name \"lounge\""));
    CHECK(rv::world::loadRoomTemplate(root, "Lounge").error().code == rv::ErrorCode::InvalidArgument);
    CHECK(rv::world::loadRoomTemplate(root, "missing").error().code == rv::ErrorCode::NotFound);

    auto platform = rv::world::loadPlatformManifest(root, *rv::catalog::SystemName::parse("nes"));
    REQUIRE_FALSE(platform.has_value());
    CHECK_THAT(platform.error().message, ContainsSubstring("the manifest is of \"snes\""));
    CHECK(rv::world::loadPlatformManifest(root, *rv::catalog::SystemName::parse("gba")).error().code ==
          rv::ErrorCode::NotFound);
}

TEST_CASE("world/data: malformed platform manifests and families are refused", "[world][data]") {
    const std::string platform = readText(kAssets / "world" / "platforms" / "snes.json");
    struct Case {
        std::string_view from;
        std::string_view to;
        std::string_view message;
    };
    for (const Case& test : {
             Case{"\"cartridge\": \"#9A9A96\"", "\"cartridge\": \"#9a9a96\"", "/plastic/cartridge: expected an sRGB"},
             Case{"\"cartridge\": \"#9A9A96\"", "\"cartridge\": \"9A9A96\"", "/plastic/cartridge"},
             Case{"\"text\": \"16-BIT CORNER\"", "\"text\": \" 16-BIT\"", "/sign/text"},
             Case{"\"text\": \"16-BIT CORNER\"", "\"text\": \"16-BIT CORNER OF THE ROOM WITH THE TV\"", "/sign/text"},
             Case{"\"system\": \"snes\"", "\"system\": \"sn/es\"", "/system: expected an ES-DE system name"},
             Case{"\"themePack\": \"classic_16bit_warm\"", "\"themePack\": \"Classic16BitWarm\"", "/themePack"},
             Case{"\"themePack\"", "\"theme\"", "/theme: unknown key"},
             Case{"\"kind\": \"console_cartridge\"", "\"kind\": \"console cartridge\"", "/console/kind"},
         }) {
        INFO(test.to);
        auto refused = rv::world::parsePlatformManifest(replaced(platform, test.from, test.to), "snes.json");
        REQUIRE_FALSE(refused.has_value());
        CHECK_THAT(refused.error().message, ContainsSubstring(std::string(test.message)));
        CHECK_THAT(refused.error().context, ContainsSubstring("snes.json:"));
    }

    const std::string families = readText(kAssets / "world" / "media_families.json");
    for (const Case& test : {
             Case{"\"spineOut\": {\"widthMm\": 20", "\"spineOut\": {\"widthMm\": 21",
                  "/families/0/spineOut: the spine-out slot must be 20 x 125 x 85 mm"},
             Case{"\"faceOut\": {\"widthMm\": 125, \"heightMm\": 85",
                  "\"faceOut\": {\"widthMm\": 85, \"heightMm\": 125",
                  "/families/0/faceOut: the face-out slot must be 125 x 85 x 20 mm"},
             Case{"\"gapMm\": 4", "\"gapMm\": -1", "/families/0/spineOut/gapMm"},
             Case{"\"itemDepthMm\": 20", "\"itemDepthMm\": 2.0", "/families/0/itemDepthMm: expected an integer"},
             Case{"\"id\": \"cartridge_home_16bit\"", "\"id\": \"cartridge-home\"", "/families/0/id"},
             Case{"\"schemaVersion\": 1", "\"schemaVersion\": 2", "/schemaVersion"},
         }) {
        INFO(test.to);
        auto refused = rv::world::parseMediaFamilies(replaced(families, test.from, test.to), "media_families.json");
        REQUIRE_FALSE(refused.has_value());
        CHECK_THAT(refused.error().message, ContainsSubstring(std::string(test.message)));
    }
    // A family twice, and none at all.
    const std::size_t start = families.find("    {\n      \"id\"");
    const std::size_t end = families.find("\n    }", start) + 6;
    const std::string family = families.substr(start, end - start);
    auto twice = rv::world::parseMediaFamilies(replaced(families, family, family + ",\n" + family), "f.json");
    REQUIRE_FALSE(twice.has_value());
    CHECK_THAT(twice.error().message, ContainsSubstring("/families/1/id: the family appears twice"));
    auto none = rv::world::parseMediaFamilies(R"({"schemaVersion": 1, "families": []})", "f.json");
    REQUIRE_FALSE(none.has_value());
    CHECK_THAT(none.error().message, ContainsSubstring("at least one family"));
}
