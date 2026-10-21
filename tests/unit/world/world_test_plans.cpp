#include "world_test_plans.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include <fmt/format.h>

#include <rv/core/text.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>

namespace rv::test {
namespace {

using world::Fixture;
using world::FixtureLevel;
using world::FixtureRole;
using world::PrefabRef;
using world::Section;
using world::SocketKind;
using world::TemplateSocket;

const std::filesystem::path kAssets = core::pathFromUtf8(RV_TEST_ASSET_DIR);

const PrefabRef kPoster{"decor.poster_frame.generic", "poster_frame"};
const PrefabRef kFloorLamp{"decor.lamp.floor.generic", "floor_lamp"};
const PrefabRef kSideboard{"furniture.tv_cabinet.generic", "tv_cabinet"};
const PrefabRef kConsole{"hardware.console.cartridge.generic", "console_cartridge"};
const PrefabRef kStorageBox{"decor.storage_box.generic", "storage_box"};
const PrefabRef kBookends{"decor.bookends.generic", "bookends"};

/// The size of each hero corner part (the test room's furniture: the manifests' scaleMeters, the armchair invented).
struct PartSize {
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::int64_t depth = 0;
};
const std::map<std::string, PartSize, std::less<>> kHeroPartSizes{
    {"tv-cabinet", {1200, 550, 500}}, {"crt", {710, 610, 560}},  {"console", {240, 70, 200}},
    {"sign", {500, 700, 20}},         {"rug", {2400, 12, 1600}}, {"armchair", {800, 900, 850}},
    {"lamp", {440, 1600, 440}},
};

/// The front of an axis-aligned yaw: 0 faces +Z (south), 9000 +X, 18000 -Z, 27000 -X.
std::pair<std::int64_t, std::int64_t> frontOf(std::int32_t yawCdeg) {
    switch (yawCdeg) {
    case 9000:
        return {1, 0};
    case 18000:
        return {0, -1};
    case 27000:
        return {-1, 0};
    default:
        return {0, 1};
    }
}

/// A fixture standing in a wall or floor socket: its back on the socket's back edge (moved south with the room for a
/// socket of the south wall), facing the socket's way, its pivot the middle of its footprint, or of its back for a
/// poster frame (hung from it: rv/geometry/prefabs.hpp).
Fixture socketFixture(const world::RoomTemplate& roomTemplate, std::uint32_t growthSteps, std::string_view socketId,
                      FixtureRole role, PrefabRef prefab, PartSize size, std::int64_t yMm = 0,
                      std::string_view part = {}) {
    const TemplateSocket* socket = roomTemplate.socket(socketId);
    REQUIRE(socket != nullptr);
    const auto [frontX, frontZ] = frontOf(socket->yawCdeg);
    const std::int64_t shift =
        socket->wall == world::Wall::South ? roomTemplate.growthStepMm * static_cast<std::int64_t>(growthSteps) : 0;
    Fixture fixture;
    fixture.id = snesFixtureId(socketId, part);
    fixture.role = role;
    fixture.prefab = std::move(prefab);
    const std::int64_t out = fixture.prefab.kind == "poster_frame" ? 0 : size.depth / 2;
    fixture.xMm = socket->xMm + frontX * out;
    fixture.yMm = yMm;
    fixture.zMm = socket->zMm + shift + frontZ * out;
    fixture.yawCdeg = socket->yawCdeg;
    fixture.widthMm = size.width;
    fixture.heightMm = size.height;
    fixture.depthMm = size.depth;
    return fixture;
}

std::vector<FixtureLevel> spineLevels(std::uint32_t levels, std::uint32_t slots) {
    std::vector<FixtureLevel> result;
    for (std::uint32_t level = 0; level < levels; ++level) {
        result.push_back({level, slots, Section::Spine});
    }
    return result;
}

constexpr PartSize kModuleSize{900, 2000, 350};
constexpr PartSize kWallShelfSize{1100, 1800, 400};
constexpr PartSize kDisplayCaseSize{1200, 900, 500};
constexpr PartSize kPosterSize{500, 700, 20};
constexpr PartSize kFloorLampSize{440, 1600, 440};
constexpr PartSize kSideboardSize{1200, 550, 500};
/// A poster's centre (its pivot) above the floor.
constexpr std::int64_t kPosterHeightMm = 1550;

world::Room roomOf(const world::RoomTemplate& roomTemplate, std::uint32_t growthSteps) {
    world::Room room;
    room.id = snesRoomId();
    room.templateId = roomTemplate.id;
    room.templateVersion = roomTemplate.version;
    const world::Footprint interior = world::interiorFootprint(roomTemplate, growthSteps);
    room.minXMm = interior.minXMm;
    room.maxXMm = interior.maxXMm;
    room.minZMm = interior.minZMm;
    room.maxZMm = interior.maxZMm;
    room.heightMm = roomTemplate.heightMm;
    room.growthSteps = growthSteps;
    room.seed = 42;
    room.mediaFamily = "cartridge_home_16bit";
    room.themePack = "classic_16bit_warm";
    // Schema 2's fields (I4 step 1): the console room of snes on gen4, at the north row's first position.
    room.kind = world::RoomKind::Console;
    room.floor = "gen4";
    room.sections = {room.id.system()};
    room.generatorVersion = {1, 0};
    room.placement = world::RoomPlacement{3000 + 150 - interior.minXMm, -1500 - 150 - interior.maxZMm, 0};
    return room;
}

world::WorldPlan emptyPlan() {
    world::WorldPlan plan;
    plan.generatorVersion = {1, 0};
    plan.randomAlgorithm = "splitmix64/1";
    plan.titleKeyRule = "title-key/1";
    plan.catalogSortKeyVersion = 1;
    plan.library = *catalog::LibraryId::parse("esde");
    plan.seed = 7;
    plan.preset = "retro_museum_cozy";
    plan.inputDigest = std::string(64, 'a');
    plan.floors = {{"ground", 0}, {"gen4", 3}};
    return plan;
}

/// The fixtures every room has, whatever its size: the hero wall (modules, hero corner, featured shelf), the display
/// cases and the south wall's sideboards.
void addFixedFixtures(world::WorldPlan& plan, const world::RoomTemplate& roomTemplate, std::uint32_t growthSteps) {
    for (std::string_view socket : {"hw-w", "hw-e"}) {
        Fixture module = socketFixture(roomTemplate, growthSteps, socket, FixtureRole::HeroModule,
                                       roomTemplate.heroModule, kModuleSize);
        module.levels = {{0, 33, Section::Spine}, {1, 33, Section::Spine}, {2, 33, Section::Spine},
                         {3, 6, Section::Band},   {4, 33, Section::Spine}, {5, 33, Section::Spine}};
        plan.fixtures.push_back(std::move(module));
    }
    const TemplateSocket* hero = roomTemplate.socket("hero");
    REQUIRE(hero != nullptr);
    for (const world::HeroPart& part : hero->parts) {
        const auto size = kHeroPartSizes.find(part.role);
        REQUIRE(size != kHeroPartSizes.end());
        Fixture fixture;
        fixture.id = snesFixtureId("hero", part.role);
        fixture.role = FixtureRole::HeroCorner;
        fixture.prefab = part.prefab.value_or(kConsole);
        // The hero corner faces south (yaw 0): its local frame is the room's, from the socket's anchor.
        fixture.xMm = hero->xMm + part.xMm;
        fixture.yMm = part.yMm;
        fixture.zMm = hero->zMm + part.zMm;
        fixture.yawCdeg = part.yawCdeg;
        fixture.widthMm = size->second.width;
        fixture.heightMm = size->second.height;
        fixture.depthMm = size->second.depth;
        plan.fixtures.push_back(std::move(fixture));
    }
    const TemplateSocket* featured = roomTemplate.socket("fs01");
    REQUIRE(featured != nullptr);
    Fixture shelf =
        socketFixture(roomTemplate, growthSteps, "fs01", FixtureRole::Display, *featured->prefab, kWallShelfSize);
    shelf.levels = {{0, 10, Section::Featured}, {1, 10, Section::Featured}};
    plan.fixtures.push_back(std::move(shelf));
    for (std::string_view socket : {"dc01", "dc02"}) {
        const TemplateSocket* templateSocket = roomTemplate.socket(socket);
        REQUIRE(templateSocket != nullptr);
        Fixture display = socketFixture(roomTemplate, growthSteps, socket, FixtureRole::Display,
                                        *templateSocket->prefab, kDisplayCaseSize);
        display.levels = {{0, 6, Section::Featured}, {1, 6, Section::Featured}};
        plan.fixtures.push_back(std::move(display));
    }
    for (std::string_view socket : {"dec-sw", "dec-se"}) {
        plan.fixtures.push_back(
            socketFixture(roomTemplate, growthSteps, socket, FixtureRole::Decoration, kSideboard, kSideboardSize));
    }
}

void addSideModule(world::WorldPlan& plan, const world::RoomTemplate& roomTemplate, std::uint32_t growthSteps,
                   std::string_view socket) {
    Fixture module =
        socketFixture(roomTemplate, growthSteps, socket, FixtureRole::SideModule, roomTemplate.sideModule, kModuleSize);
    module.levels = spineLevels(7, 33);
    plan.fixtures.push_back(std::move(module));
}

} // namespace

world::RoomTemplate shippedRoomTemplate() {
    auto roomTemplate = world::loadRoomTemplate(kAssets, "console_room_9x10");
    REQUIRE(roomTemplate.has_value());
    return *roomTemplate;
}

catalog::GameId testGameId(std::uint64_t value) {
    return *catalog::GameId::fromValue(value);
}

world::RoomId snesRoomId() {
    return *world::RoomId::forSystem(*catalog::SystemName::parse("snes"));
}

world::FixtureId snesFixtureId(std::string_view socket, std::string_view part) {
    auto id = world::FixtureId::make(snesRoomId(), socket, part);
    REQUIRE(id.has_value());
    return *id;
}

world::SlotId snesSlotId(std::string_view socket, std::uint32_t level, std::uint32_t index) {
    auto id = world::SlotId::make(snesFixtureId(socket), level, index);
    REQUIRE(id.has_value());
    return *id;
}

world::WorldPlan validRoomPlan() {
    const world::RoomTemplate roomTemplate = shippedRoomTemplate();
    world::WorldPlan plan = emptyPlan();
    plan.rooms.push_back(roomOf(roomTemplate, 0));
    addFixedFixtures(plan, roomTemplate, 0);
    for (std::string_view socket : {"w01", "w02", "w03", "e01", "e02", "e03"}) {
        addSideModule(plan, roomTemplate, 0, socket);
    }
    for (std::string_view socket : {"w04", "w06", "w07", "e04", "e06", "e07"}) {
        plan.fixtures.push_back(socketFixture(roomTemplate, 0, socket, world::FixtureRole::Decoration, kPoster,
                                              kPosterSize, kPosterHeightMm));
    }
    for (std::string_view socket : {"w05", "e05"}) {
        plan.fixtures.push_back(
            socketFixture(roomTemplate, 0, socket, world::FixtureRole::Decoration, kFloorLamp, kFloorLampSize));
    }

    // Games 1-10 on the west hero module's lowest spine level, 11-16 in its band, 17-26 on the featured shelf, 27-30
    // on w01's lowest level.
    std::uint64_t game = 1;
    for (std::uint32_t index = 0; index < 10; ++index) {
        plan.placements.push_back({snesSlotId("hw-w", 0, index), testGameId(game++), 2, 0});
    }
    for (std::uint32_t index = 0; index < 6; ++index) {
        plan.placements.push_back({snesSlotId("hw-w", 3, index), testGameId(game++), 0, 100});
    }
    for (std::uint32_t index = 0; index < 10; ++index) {
        plan.placements.push_back({snesSlotId("fs01", 0, index), testGameId(game++), 0, -50});
    }
    for (std::uint32_t index = 0; index < 4; ++index) {
        plan.placements.push_back({snesSlotId("w01", 0, index), testGameId(game++), 1, 0});
    }
    plan.dressing = {{*world::LevelId::make(snesFixtureId("hw-w"), 0), 20, 13, kBookends},
                     {*world::LevelId::make(snesFixtureId("w03"), 6), 0, 33, kStorageBox}};
    plan.unplaced = {{testGameId(101), snesRoomId(), world::UnplacedReason::DuplicateTitle, testGameId(1)},
                     {testGameId(102), snesRoomId(), world::UnplacedReason::NoCover, {}}};
    return plan;
}

world::WorldPlan grownRoomPlan(std::uint32_t growthSteps) {
    const world::RoomTemplate roomTemplate = shippedRoomTemplate();
    world::WorldPlan plan = emptyPlan();
    plan.rooms.push_back(roomOf(roomTemplate, growthSteps));
    addFixedFixtures(plan, roomTemplate, growthSteps);
    for (const TemplateSocket& socket : roomTemplate.sockets) {
        if (socket.kind == SocketKind::SideWall && socket.growthStep <= growthSteps) {
            addSideModule(plan, roomTemplate, growthSteps, socket.id);
        }
    }
    std::uint64_t game = 1;
    for (const Fixture& fixture : std::vector<Fixture>(plan.fixtures)) {
        if (fixture.role != FixtureRole::SideModule) {
            continue;
        }
        for (const FixtureLevel& level : fixture.levels) {
            for (std::uint32_t index = 0; index + 4 < level.slots && game <= 2'000; ++index) {
                plan.placements.push_back(
                    {*world::SlotId::make(fixture.id, level.level, index), testGameId(game++), 0, 0});
            }
        }
    }
    return plan;
}

world::Fixture& fixtureOf(world::WorldPlan& plan, std::string_view socket, std::string_view part) {
    const world::FixtureId id = snesFixtureId(socket, part);
    const auto found = std::find_if(plan.fixtures.begin(), plan.fixtures.end(),
                                    [&](const Fixture& fixture) { return fixture.id == id; });
    REQUIRE(found != plan.fixtures.end());
    return *found;
}

world::RoomSelection selectionOf(const world::WorldPlan& plan) {
    world::RoomSelection selection;
    selection.room = snesRoomId();
    selection.titleKeyRule = "title-key/1";
    selection.catalogSortKeyVersion = 1;
    for (const world::Placement& placement : plan.placements) {
        world::RoomTitle title;
        title.game = placement.game;
        title.titleKey = fmt::format("title {:05}", placement.game.value());
        title.sortKey = title.titleKey;
        selection.titles.push_back(std::move(title));
    }
    std::sort(selection.titles.begin(), selection.titles.end(), world::priorityLess);
    selection.unplaced = plan.unplaced;
    selection.distinctTitles = selection.titles.size();
    return selection;
}

world::PrefabSlotSource prefabSlotsOf(const world::WorldPlan& plan) {
    auto slots = std::make_shared<std::map<PrefabRef, std::vector<world::PrefabSlotLevel>>>();
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.levels.empty()) {
            continue;
        }
        std::vector<world::PrefabSlotLevel>& levels = (*slots)[fixture.prefab];
        levels.clear();
        for (const FixtureLevel& level : fixture.levels) {
            levels.push_back({level.level, level.slots});
        }
    }
    return [slots](const PrefabRef& prefab) -> Expected<std::vector<world::PrefabSlotLevel>> {
        const auto found = slots->find(prefab);
        if (found == slots->end()) {
            return makeError(ErrorCode::NotFound, "no such prefab in the test", prefab.manifest);
        }
        return found->second;
    };
}

world::ValidationContext validContextFor(const world::WorldPlan& plan) {
    world::ValidationContext context;
    context.templates.push_back(shippedRoomTemplate());
    auto preset = world::loadPreset(kAssets, "retro_museum_cozy");
    REQUIRE(preset.has_value());
    context.config = *preset;
    context.selections.push_back(selectionOf(plan));
    context.prefabSlots = prefabSlotsOf(plan);
    context.lights.push_back({snesRoomId(), 12});
    return context;
}

} // namespace rv::test
