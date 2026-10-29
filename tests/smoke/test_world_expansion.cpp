// Expansion rooms in the app (I4b step 8; ADR-0015 §4, §7 as amended): the esde/overflow fixture (1,700 big boxes of
// amiga, 60 arcade sets of mame) planned fresh by retroverse-cli (generator 1.5: room:amiga and room:amiga:2 of 850
// titles each, facing each other across the computer floor's hall; area:arcade and area:arcade:2 of 30 on the ground
// floor's hub), retroverse.exe hidden, lockstep (--fixed-dt), RV_AUDIO=null (runApp()):
//  - the arcade's 60 titles all placed, nothing unplaced for capacity;
//  - the hall's portals of the head and its :2 (screenshot), a walk from the hall into room:amiga:2 (the eye's room at
//    the end, its CRT playing its own games), a cover of room:amiga:2 inspected;
//  - the directory's computer floor with the chain (screenshot), a teleport into room:amiga:2 from it, "Mostra nella
//    stanza" of one of its games from the head (a teleport, then the game's Focus pose);
//  - the editor: "Sposta" on the card of a display case's game of the head, the game carried through the hall into
//    room:amiga:2 and dropped into a free slot there, Salva (the stored plan holds it there, pinned by the editor);
//  - an update that adds an expansion (the arcade planned in one room at its limit, 20 new sets laid over the library):
//    the notice names "Sala giochi arcade 2".
// Screenshots under build/<preset>/smoke/world_expansion/*/ (docs/i4b/step-8.md copies them to build/i4b/step8/).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/command_line.hpp>
#include <rv/app/room_items.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_edit_room.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "building_support.hpp"
#include "image_checks.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
constexpr glm::vec2 kWindowSize{1280.0f, 720.0f};
constexpr std::string_view kHead = "room:amiga";
constexpr std::string_view kExpansion = "room:amiga:2";
/// The walker's eye above the feet, the mouse's look per pixel, the walking speed (scene::FirstPersonSettings').
constexpr float kEyeHeight = 1.65f;
constexpr float kRadiansPerPixel = 0.0022f;
constexpr float kWalkSpeed = 2.5f;

fs::path buildAssets() {
    return buildConfig().devAssetRoot ? sourceDir() / "assets" : retroverseExe().parent_path() / "assets";
}

Json press(std::uint64_t frame, std::string_view action) {
    return Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{std::string(action), 1}}}};
}

Json look(std::uint64_t frame, glm::vec2 pixels) {
    return Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{"look_delta_x", pixels.x}, {"look_delta_y", pixels.y}}}};
}

/// A straight walk of `meters` from `frame` on, at most 80% of the walking speed, its distance exact: the frame after it.
std::uint64_t walk(Json& script, std::uint64_t frame, float meters) {
    const int frames = std::max(1, static_cast<int>(std::ceil(meters * 60.0f / (kWalkSpeed * 0.8f))));
    const float axis = meters * 60.0f / (kWalkSpeed * static_cast<float>(frames));
    script.push_back(Json{{"fromFrame", frame},
                          {"toFrame", frame + static_cast<std::uint64_t>(frames) - 1},
                          {"actions", {{"move_forward", axis}}}});
    return frame + static_cast<std::uint64_t>(frames);
}

fs::path writeScript(const PlannedBuilding& building, std::string_view name, const Json& script) {
    const fs::path file = building.dir / (std::string(name) + ".json");
    REQUIRE(rv::core::writeTextFileAtomic(file, script.dump(2) + "\n").has_value());
    return file;
}

/// The yaw and pitch that look along `direction` (scene::Camera's: yaw 0 towards -Z, positive to the right and up).
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

rv::app::SpawnPoint spawnAt(const glm::vec3& feet, glm::vec2 view) {
    rv::app::SpawnPoint spawn;
    spawn.feet = feet;
    spawn.yawDeg = rv::core::radiansToDegrees(view.x);
    spawn.pitchDeg = rv::core::radiansToDegrees(view.y);
    return spawn;
}

/// A room as the app draws it with --start-room `start`: the scene's frame is the start room's (the app's
/// relativePlacement()); its collection and doorway camera moved into it.
struct RoomView {
    rv::world::Room room;
    rv::world::SceneData data;
    rv::world::CollectionLayout layout;
    /// The doorway camera's eye (inside the doorway, looking into the room) and the direction it looks.
    glm::vec3 doorwayEye{0.0f};
    glm::vec3 inward{0.0f};
    rv::app::FocusViewer viewer;
};

RoomView roomView(const rv::world::WorldPlan& plan, std::string_view id, std::string_view start) {
    RoomView view;
    view.room = roomIn(plan, id);
    const rv::world::Room& origin = roomIn(plan, start);
    rv::world::RoomPlacement relative;
    const std::int64_t sign = origin.placement.yawHalfTurns % 2 == 0 ? 1 : -1;
    relative.xMm = sign * (view.room.placement.xMm - origin.placement.xMm);
    relative.zMm = sign * (view.room.placement.zMm - origin.placement.zMm);
    relative.yawHalfTurns = (view.room.placement.yawHalfTurns + origin.placement.yawHalfTurns) % 2;
    auto data = rv::world::loadSceneData(buildAssets(), plan, view.room.id);
    REQUIRE(data.has_value());
    view.data = std::move(*data);
    auto scene = rv::world::toSceneDescription(plan, view.room.id, view.data);
    REQUIRE(scene.has_value());
    view.viewer.lens = scene->camera.lens;
    view.viewer.aspect = kWindowSize.x / kWindowSize.y;
    auto layout = rv::world::collectionLayout(plan, view.room.id, view.data, {});
    REQUIRE(layout.has_value());
    view.layout = rv::world::floorCollectionLayout(std::move(*layout), relative);
    const rv::world::DoorwayCamera camera = rv::world::doorwayCamera(view.room, view.data.roomTemplate);
    const glm::mat4 toScene = rv::world::roomToFloorMatrix(relative);
    view.doorwayEye = glm::vec3(toScene * glm::vec4(static_cast<float>(camera.x), static_cast<float>(camera.eyeY),
                                                    static_cast<float>(camera.z), 1.0f));
    const float yaw = rv::core::degreesToRadians(static_cast<float>(camera.yawDeg));
    view.inward = glm::normalize(glm::vec3(toScene * glm::vec4(std::sin(yaw), 0.0f, -std::cos(yaw), 0.0f)));
    return view;
}

/// Where the walker may stand in a room: its capsule (0.3 m and a margin) clear of every occluder of its layout and of
/// every slot (a display case's glass is no occluder but stops the walker).
struct Walkable {
    std::vector<rv::core::Aabb> solids;

    explicit Walkable(const rv::world::CollectionLayout& layout) : solids(layout.occluders) {
        for (const rv::world::CollectionItem& item : layout.items) {
            solids.push_back(item.worldBounds());
        }
        for (const rv::world::FreeSlot& free : layout.freeSlots) {
            solids.push_back(free.worldBounds());
        }
    }

    [[nodiscard]] bool standable(const glm::vec3& feet) const {
        constexpr float kClear = 0.45f;
        return std::none_of(solids.begin(), solids.end(), [&](const rv::core::Aabb& box) {
            return box.max.y > feet.y + 0.05f && box.min.y < feet.y + 1.8f && feet.x > box.min.x - kClear &&
                   feet.x < box.max.x + kClear && feet.z > box.min.z - kClear && feet.z < box.max.z + kClear;
        });
    }

    [[nodiscard]] bool clearWalk(const glm::vec3& from, const glm::vec3& to) const {
        const int steps = static_cast<int>(std::ceil(glm::length(to - from) / 0.05f));
        for (int step = 0; step <= steps; ++step) {
            if (!standable(glm::mix(from, to, static_cast<float>(step) / static_cast<float>(std::max(steps, 1))))) {
                return false;
            }
        }
        return true;
    }
};

glm::vec3 feetOf(const glm::vec3& eye) {
    return {eye.x, 0.0f, eye.z};
}

/// The title of `game` in `system`'s room items, lower case (what the quick catalog's search finds it by), and whether
/// no other title of the fixture's items holds it.
std::optional<std::string> uniqueSearch(const PlannedBuilding& building, const rv::catalog::GameId& game) {
    const rv::catalog::LibraryId library = rv::catalog::LibraryId::parse("esde").value();
    std::vector<rv::app::RoomItems> all;
    for (const std::string_view system : {"amiga", "mame"}) {
        auto items = rv::app::readRoomItems(
            rv::app::roomItemsFile(building.data / "cache", library, rv::catalog::SystemName::parse(system).value()));
        if (items) {
            all.push_back(std::move(*items));
        }
    }
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        return text;
    };
    std::optional<std::string> search;
    for (const rv::app::RoomItems& items : all) {
        if (const rv::app::RoomItemText* text = items.find(game); text != nullptr && !text->title.empty()) {
            search = lower(text->title);
        }
    }
    if (!search) {
        return std::nullopt;
    }
    for (const rv::app::RoomItems& items : all) {
        for (const rv::app::RoomItemText& text : items.items) {
            if (text.game != game && lower(text.title).find(*search) != std::string::npos) {
                return std::nullopt;
            }
        }
    }
    return search;
}

std::vector<std::string> roomArguments(std::string_view room, std::uint32_t frames,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", std::string(room), "--exit-after-frames", "--frames",
                                       std::to_string(frames), "--fixed-dt", "0.016667"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

} // namespace

TEST_CASE("world_expansion: the chains of a fresh building, every arcade set placed", "[world][expansion]") {
    const PlannedBuilding building("world_expansion/chains", true, "esde/overflow");
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::WorldPlan& plan = stored.plan;
    CHECK(placementsIn(plan, kHead) == 850);
    CHECK(placementsIn(plan, kExpansion) == 850);
    CHECK(roomIn(plan, kExpansion).expands.toString() == kHead);
    // The arcade's 60 titles all placed, in two rooms of 30: none unplaced for capacity.
    CHECK(placementsIn(plan, "area:arcade") + placementsIn(plan, "area:arcade:2") == 60);
    CHECK(std::none_of(plan.unplaced.begin(), plan.unplaced.end(), [](const rv::world::UnplacedGame& game) {
        return game.reason == rv::world::UnplacedReason::Capacity;
    }));
    // In the app: the arcade's expansion is a room of the ground floor like any (its doorway the start), the plan as
    // stored (the update of an unchanged catalog writes nothing).
    const fs::path shot = building.dir / "arcade-expansion.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "arcade",
                              roomArguments("area:arcade:2", 30, {"--screenshot", arg(shot)}));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->world.state == "room");
    CHECK(run.report->world.room == "area:arcade:2");
    CHECK(run.report->world.capacity == 0);
    CHECK_FALSE(run.report->world.planWritten);
    CHECK(run.report->world.roomsAdded.empty());
    static_cast<void>(requireScreenshot(run, shot, kShotSize));
}

TEST_CASE("world_expansion: the hall's portals, a walk from the hall into room:amiga:2, a cover inspected there",
          "[world][expansion]") {
    const PlannedBuilding building("world_expansion/walk", true, "esde/overflow");
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::WorldPlan& plan = stored.plan;
    const RoomView head = roomView(plan, kHead, kHead);
    const RoomView expansion = roomView(plan, kExpansion, kHead);
    const glm::vec3 p1 = feetOf(head.doorwayEye);
    const glm::vec3 p2 = feetOf(expansion.doorwayEye);
    const glm::vec3 across = glm::normalize(p2 - p1);
    INFO("the head's doorway at (" << p1.x << ", " << p1.z << "), the expansion's at (" << p2.x << ", " << p2.z << ")");
    // The two doorways face each other across the hall (the expansion entered the south row at the head's x).
    CHECK(std::abs(p1.x - p2.x) < 0.01f);
    CHECK(glm::dot(head.inward, -across) > 0.99f);
    CHECK(glm::dot(expansion.inward, across) > 0.99f);

    // The hall: both portals and their signs, from down the hall.
    const glm::vec3 middle = (p1 + p2) * 0.5f;
    const glm::vec3 hallEye = middle + glm::vec3(4.0f, 0.0f, 0.0f);
    const glm::vec3 signs = middle + glm::vec3(0.0f, 2.6f, 0.0f);
    const fs::path hallShot = building.dir / "hall-portals.png";
    const AppRun hall = runApp(retroverseExe(), building.dir, "hall",
                               roomArguments(kHead, 20,
                                             {"--spawn",
                                              spawnArgument(spawnAt(hallEye,
                                                                    lookAlong(glm::normalize(
                                                                        signs - hallEye - glm::vec3(0.0f, kEyeHeight, 0.0f))))),
                                              "--screenshot", arg(hallShot)}));
    skipWithoutOpenGl(hall);
    requireCleanRun(hall);
    INFO(hall.describe());
    CHECK(hall.report->world.room.empty()); // in the hall
    CHECK(hall.report->world.floor == "computer");
    static_cast<void>(requireScreenshot(hall, hallShot, kShotSize));

    // The walk: from the hall a metre out of the head's doorway, straight across into the expansion, 1.8 m past its
    // doorway; then a second for the CRT's dwell.
    Json script = Json::array();
    const float meters = glm::length(p2 - p1) - 1.0f + 1.8f;
    const std::uint64_t walked = walk(script, 20, meters);
    const std::uint64_t frames = walked + 60;
    const fs::path walkShot = building.dir / "inside-expansion.png";
    const AppRun walkRun =
        runApp(retroverseExe(), building.dir, "walk",
               roomArguments(kHead, static_cast<std::uint32_t>(frames),
                             {"--spawn", spawnArgument(spawnAt(p1 + across * 1.0f, lookAlong(across))), "--input-script",
                              arg(writeScript(building, "walk_script", script)), "--screenshot", arg(walkShot)}));
    requireCleanRun(walkRun);
    INFO(walkRun.describe());
    const rv::app::WorldRecord& world = walkRun.report->world;
    CHECK(world.room == kExpansion);
    CHECK(world.floor == "computer");
    REQUIRE(walkRun.report->finalCamera.has_value());
    CHECK(glm::length(walkRun.report->finalCamera->feet - (p2 + across * 1.8f)) < 0.1f);
    // Its CRT plays its own games: the eye's room's (ADR-0013 §9, unchanged).
    CHECK(world.screenRoom == kExpansion);
    CHECK(walkRun.report->video.playlistRoom == kExpansion);
    std::set<std::string> ownGames;
    for (const rv::world::Placement& placement : plan.placements) {
        if (placement.slot.fixture().room().toString() == kExpansion) {
            ownGames.insert(placement.game.toString());
        }
    }
    for (const std::string& game : walkRun.report->video.playlistOrder) {
        INFO(game);
        CHECK(ownGames.contains(game));
    }
    static_cast<void>(requireScreenshot(walkRun, walkShot, kShotSize));

    // A cover of the expansion inspected: from a display case game's Focus pose, interact (a face-out item: Inspect at
    // once), its cover on the item in the hand.
    const RoomView own = roomView(plan, kExpansion, kExpansion);
    const auto faceOut = std::find_if(own.layout.items.begin(), own.layout.items.end(), [](const auto& item) {
        return item.stance == rv::world::ItemStance::FaceOut;
    });
    REQUIRE(faceOut != own.layout.items.end());
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(*faceOut, own.viewer);
    Json inspectScript = Json::array({press(40, "interact")});
    const fs::path inspectShot = building.dir / "inspect-expansion.png";
    const AppRun inspect = runApp(
        retroverseExe(), building.dir, "inspect",
        roomArguments(kExpansion, 130,
                      {"--spawn", spawnArgument(spawnAt(pose.feet, {pose.yaw, pose.pitch})), "--input-script",
                       arg(writeScript(building, "inspect_script", inspectScript)), "--screenshot", arg(inspectShot)}));
    requireCleanRun(inspect);
    INFO(inspect.describe());
    const rv::app::InteractionRecord& interaction = inspect.report->world.interaction;
    CHECK(interaction.state == "inspect");
    CHECK(interaction.inspect.inspections == 1);
    CHECK(interaction.inspect.heroFront);
    CHECK(inspect.report->world.card.game == faceOut->game.toString());
    CHECK(inspect.report->world.room == kExpansion);
    static_cast<void>(requireScreenshot(inspect, inspectShot, kShotSize));
}

TEST_CASE("world_expansion: the directory lists the chain; a teleport and \"Mostra nella stanza\" lead into the "
          "expansion",
          "[world][expansion]") {
    const PlannedBuilding building("world_expansion/directory", true, "esde/overflow");
    const rv::world::StoredPlan stored = building.plan();
    const rv::world::WorldPlan& plan = stored.plan;

    // The directory's computer floor (the walker's), its rooms: the hall, the head, the expansion under it.
    Json script = Json::array({press(20, "map"), press(26, "ui_activate"), press(32, "ui_down")});
    const fs::path directoryShot = building.dir / "directory-chain.png";
    const AppRun directory = runApp(retroverseExe(), building.dir, "directory",
                                    roomArguments(kHead, 50,
                                                  {"--input-script", arg(writeScript(building, "directory_script", script)),
                                                   "--screenshot", arg(directoryShot)}));
    skipWithoutOpenGl(directory);
    requireCleanRun(directory);
    INFO(directory.describe());
    CHECK(directory.report->directory.open);
    CHECK(directory.report->directory.view == "rooms");
    static_cast<void>(requireScreenshot(directory, directoryShot, kShotSize));

    // The expansion's row chosen: a teleport to its doorway.
    script.push_back(press(38, "ui_activate"));
    const AppRun teleport = runApp(retroverseExe(), building.dir, "teleport",
                                   roomArguments(kHead, 70, {"--input-script",
                                                             arg(writeScript(building, "teleport_script", script))}));
    requireCleanRun(teleport);
    INFO(teleport.describe());
    REQUIRE(teleport.report->world.teleports.size() == 1);
    CHECK(teleport.report->world.teleports.front().to == kExpansion);
    CHECK(teleport.report->world.teleports.front().reason == "room");
    CHECK(teleport.report->world.room == kExpansion);
    CHECK(teleport.report->world.rides.empty());

    // "Mostra nella stanza" of a display case's game of the expansion (its title unique in the fixture), asked from the
    // head: the quick catalog, its search, its card; a teleport into the expansion, then the game's Focus pose.
    const RoomView own = roomView(plan, kExpansion, kExpansion);
    std::optional<rv::world::CollectionItem> target;
    std::string search;
    for (const rv::world::CollectionItem& item : own.layout.items) {
        if (item.stance != rv::world::ItemStance::FaceOut) {
            continue;
        }
        if (const std::optional<std::string> unique = uniqueSearch(building, item.game)) {
            target = item;
            search = *unique;
            break;
        }
    }
    REQUIRE(target.has_value());
    Json show = Json::array({press(5, "quick_catalog"), press(8, "ui_up"), press(11, "ui_up"),
                             Json{{"fromFrame", 14}, {"toFrame", 14}, {"text", search}}, press(17, "ui_activate"),
                             press(20, "ui_right"), press(23, "ui_down"), press(26, "ui_activate")});
    const fs::path showShot = building.dir / "show-in-expansion.png";
    const AppRun shown = runApp(retroverseExe(), building.dir, "show",
                                roomArguments(kHead, 100,
                                              {"--input-script", arg(writeScript(building, "show_script", show)),
                                               "--screenshot", arg(showShot)}));
    requireCleanRun(shown);
    INFO(shown.describe());
    REQUIRE(shown.report->catalog.has_value());
    CHECK(shown.report->catalog->selectedId == target->game.toString());
    REQUIRE(shown.report->world.teleports.size() == 1);
    CHECK(shown.report->world.teleports.front().to == kExpansion);
    CHECK(shown.report->world.teleports.front().reason == "showInRoom");
    CHECK(shown.report->world.launch.showInRoom == 1);
    CHECK(shown.report->world.interaction.state == "focus");
    CHECK(shown.report->world.interaction.focusedGame == target->game.toString());
    CHECK(shown.report->world.room == kExpansion);
    static_cast<void>(requireScreenshot(shown, showShot, kShotSize));
}

TEST_CASE("world_expansion: the editor carries a game of the head through the hall into its expansion",
          "[world][expansion][edit]") {
    const PlannedBuilding building("world_expansion/edit", true, "esde/overflow");
    const rv::world::StoredPlan before = building.plan();
    const rv::world::WorldPlan& plan = before.plan;
    const RoomView head = roomView(plan, kHead, kHead);
    const RoomView expansion = roomView(plan, kExpansion, kHead);
    const Walkable headFloor(head.layout);
    const Walkable expansionFloor(expansion.layout);
    auto headTargets = rv::app::EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(head.layout));
    REQUIRE(headTargets.has_value());
    auto expansionTargets =
        rv::app::EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(expansion.layout));
    REQUIRE(expansionTargets.has_value());
    const glm::vec3 up(0.0f, kEyeHeight, 0.0f);
    const auto seen = [&](const rv::app::EditRoom& targets, const glm::vec3& eye, const glm::vec3& at,
                          rv::world::PickId id) {
        const glm::vec3 to = at - eye;
        if (glm::length(to) > rv::world::kMaxPickDistance - 0.3f) {
            return false;
        }
        const rv::world::HoverSample sample =
            targets.index().sample(rv::core::Ray{eye, glm::normalize(to)}, rv::world::kNoPick);
        return sample.hit == id && sample.onCore;
    };
    // The way through the hall: a metre inside the head's doorway, straight across to a metre inside the expansion's.
    const glm::vec3 headInside = feetOf(head.doorwayEye) + head.inward * 1.0f;
    const glm::vec3 expansionInside = feetOf(expansion.doorwayEye) + expansion.inward * 1.0f;
    REQUIRE(headFloor.standable(headInside));
    REQUIRE(expansionFloor.standable(expansionInside));
    // A: a face-out game of the head (its card shows at once), seen from a place with a clear walk to the doorway.
    std::optional<rv::world::CollectionItem> a;
    glm::vec3 start{0.0f};
    for (const rv::world::CollectionItem& item : head.layout.items) {
        if (item.stance != rv::world::ItemStance::FaceOut || a) {
            continue;
        }
        const glm::vec3 at = item.worldBounds().center();
        float best = 1e9f;
        for (float dx = -4.5f; dx <= 4.5f; dx += 0.1f) {
            for (float dz = -5.0f; dz <= 5.0f; dz += 0.1f) {
                const glm::vec3 feet = headInside + glm::vec3(dx, 0.0f, dz);
                const float way = glm::length(headInside - feet);
                if (way >= best || way < 0.5f || !headFloor.standable(feet) ||
                    !seen(**headTargets, feet + up, at, item.pickId) || !headFloor.clearWalk(feet, headInside)) {
                    continue;
                }
                best = way;
                start = feet;
                a = item;
            }
        }
    }
    INFO("a face-out game of the head seen from a place with a clear walk to its doorway");
    REQUIRE(a.has_value());
    // F: a free slot of the expansion whose Focus pose has a clear walk from inside its doorway.
    std::optional<rv::world::FreeSlot> free;
    std::optional<rv::scene::WalkerPose> there;
    float nearest = 1e9f;
    for (const rv::world::FreeSlot& slot : expansion.layout.freeSlots) {
        const std::optional<rv::scene::WalkerPose> pose = (*expansionTargets)->focusPose(slot.pickId, expansion.viewer);
        if (!pose || !expansionFloor.standable(pose->feet) ||
            !seen(**expansionTargets, pose->feet + up, slot.worldBounds().center(), slot.pickId) ||
            !expansionFloor.clearWalk(expansionInside, pose->feet)) {
            continue;
        }
        const float way = glm::length(pose->feet - expansionInside);
        if (way < nearest) {
            nearest = way;
            free = slot;
            there = pose;
        }
    }
    INFO("a free slot of the expansion with a clear walk from its doorway");
    REQUIRE(free.has_value());
    INFO("A " << a->slot.toString() << " (" << a->game.toString() << "), F " << free->slot.toString());

    // frame 40 interact: Inspect, the card with Avvia focused; 70, 76 ui_down: Informazioni, Sposta; 82 ui_activate:
    // the editor, A in the hand. Then the turns and walks: to inside the head's doorway, across the hall to inside the
    // expansion's (the editor edits it from there, A carried), to F's Focus pose, onto F; interact: A moved there;
    // Enter: the menu, Salva focused; ui_activate: Salva.
    const glm::vec2 toA = lookAlong(glm::normalize(a->worldBounds().center() - start - up));
    const glm::vec2 heading1(lookAlong(glm::normalize(headInside - start)).x, toA.y);
    const glm::vec2 heading2(lookAlong(glm::normalize(expansionInside - headInside)).x, toA.y);
    const glm::vec2 heading3(lookAlong(glm::normalize(there->feet - expansionInside)).x, toA.y);
    Json script = Json::array({press(40, "interact"), press(70, "ui_down"), press(76, "ui_down"),
                               press(82, "ui_activate"), look(100, turnPixels(toA, heading1))});
    std::uint64_t frame = walk(script, 101, glm::length(headInside - start));
    script.push_back(look(frame + 2, turnPixels(heading1, heading2)));
    frame = walk(script, frame + 3, glm::length(expansionInside - headInside));
    const std::uint64_t carried = frame + 10; // in the expansion, A in the hand
    script.push_back(look(frame + 2, turnPixels(heading2, heading3)));
    frame = walk(script, frame + 3, glm::length(there->feet - expansionInside));
    const std::uint64_t turn = frame + 3;
    script.push_back(look(turn, turnPixels(heading3, glm::vec2(there->yaw, there->pitch))));
    const std::uint64_t holding = turn + 15; // A over F, outlined where it would stand
    script.push_back(press(turn + 20, "interact"));
    script.push_back(Json{{"fromFrame", turn + 50}, {"toFrame", turn + 50}, {"actions", {{"confirm", 1}, {"ui_activate", 1}}}});
    script.push_back(press(turn + 70, "ui_activate"));
    const fs::path scriptFile = writeScript(building, "edit_script", script);
    const std::vector<std::string> spawn{"--spawn", spawnArgument(spawnAt(start, toA)), "--input-script",
                                         arg(scriptFile)};

    // The bar in the expansion with A in the hand, then over F: runs that stop before the drop.
    const fs::path carryShot = building.dir / "carried-into-expansion.png";
    std::vector<std::string> carryArguments = spawn;
    carryArguments.insert(carryArguments.end(), {"--screenshot", arg(carryShot)});
    const AppRun carry = runApp(retroverseExe(), building.dir, "carry",
                                roomArguments(kHead, static_cast<std::uint32_t>(carried), carryArguments));
    skipWithoutOpenGl(carry);
    requireCleanRun(carry);
    INFO(carry.describe());
    REQUIRE(carry.report->world.edits.size() == 1);
    CHECK(carry.report->world.edits.front().rooms == std::vector<std::string>{std::string(kHead), std::string(kExpansion)});
    CHECK(carry.report->world.edits.front().operations.empty());
    CHECK(carry.report->world.room == kExpansion);
    static_cast<void>(requireScreenshot(carry, carryShot, kShotSize));
    const fs::path holdingShot = building.dir / "holding-over-free-slot.png";
    std::vector<std::string> holdingArguments = spawn;
    holdingArguments.insert(holdingArguments.end(), {"--screenshot", arg(holdingShot)});
    const AppRun hold = runApp(retroverseExe(), building.dir, "holding",
                               roomArguments(kHead, static_cast<std::uint32_t>(holding), holdingArguments));
    requireCleanRun(hold);
    static_cast<void>(requireScreenshot(hold, holdingShot, kShotSize));
    CHECK(building.plan().digest == before.digest);

    // The whole session, saved.
    const fs::path savedShot = building.dir / "moved-into-expansion.png";
    std::vector<std::string> savedArguments = spawn;
    savedArguments.insert(savedArguments.end(), {"--screenshot", arg(savedShot)});
    const AppRun run =
        runApp(retroverseExe(), building.dir, "session", roomArguments(kHead, static_cast<std::uint32_t>(turn + 110), savedArguments));
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.edits.size() == 1);
    const rv::app::WorldEditRecord& edit = run.report->world.edits.front();
    CHECK(edit.room == kHead);
    CHECK(edit.via == "card");
    CHECK(edit.heldAtStart == a->game.toString());
    CHECK(edit.rooms == std::vector<std::string>{std::string(kHead), std::string(kExpansion)});
    CHECK(edit.end == "saved");
    CHECK(edit.written);
    REQUIRE(edit.operations.size() == 1);
    CHECK(edit.operations.front().operation == "move " + a->game.toString() + " to " + free->slot.toString());
    CHECK(edit.operations.front().refused.empty());
    // In the expansion the game keeps its title and its file (the reticle on it at the end: its label).
    const auto items = rv::app::readRoomItems(rv::app::roomItemsFile(
        building.data / "cache", rv::catalog::LibraryId::parse("esde").value(),
        rv::catalog::SystemName::parse("amiga").value()));
    REQUIRE(items.has_value());
    REQUIRE(items->find(a->game) != nullptr);
    CHECK(run.report->world.hoveredGame == a->game.toString());
    CHECK(run.report->world.interaction.label == items->find(a->game)->title);
    static_cast<void>(requireScreenshot(run, savedShot, kShotSize));
    // The stored plan: A in the expansion's slot, pinned there by the editor; the head one game fewer.
    const rv::world::StoredPlan after = building.plan();
    CHECK(after.digest != before.digest);
    const auto placed = std::find_if(after.plan.placements.begin(), after.plan.placements.end(),
                                     [&](const rv::world::Placement& placement) { return placement.game == a->game; });
    REQUIRE(placed != after.plan.placements.end());
    CHECK(placed->slot == free->slot);
    CHECK(placementsIn(after.plan, kHead) == 849);
    CHECK(placementsIn(after.plan, kExpansion) == 851);
    auto overrides = rv::world::loadOverrides(
        rv::world::worldFiles(building.data, rv::catalog::LibraryId::parse("esde").value()).overrides);
    REQUIRE(overrides.has_value());
    REQUIRE(overrides->gamePins.size() == 1);
    CHECK(overrides->gamePins.front().game == a->game);
    CHECK(overrides->gamePins.front().slot == free->slot);
    CHECK(overrides->gamePins.front().provenance.origin == rv::world::PinOrigin::Editor);
}

TEST_CASE("world_expansion: an update that adds an expansion names the new room in the notice", "[world][expansion]") {
    // The arcade's 60 sets planned without a growth reserve: one room grown to its template's limit (I4b step 7's
    // review's kRoomAtLimitWorldJson). Then 20 new sets (their ROMs and covers) laid over the library and imported: the
    // app's update fills the head's last free slots and the chain takes area:arcade:2 (ADR-0015 §7).
    const PlannedBuilding building("world_expansion/notice", true, "esde/overflow", {}, kRoomAtLimitWorldJson);
    const rv::world::StoredPlan planned = building.plan();
    REQUIRE(placementsIn(planned.plan, "area:arcade") == 60);
    REQUIRE(std::none_of(planned.plan.rooms.begin(), planned.plan.rooms.end(),
                         [](const rv::world::Room& room) { return room.id.toString() == "area:arcade:2"; }));
    fs::path cover;
    for (const fs::directory_entry& entry : fs::directory_iterator(building.fixture.path("media/mame/covers"))) {
        if (entry.is_regular_file() && cover.empty()) {
            cover = entry.path();
        }
    }
    REQUIRE_FALSE(cover.empty());
    for (int index = 1; index <= 20; ++index) {
        const std::string name = "zzextra" + std::to_string(index);
        REQUIRE(rv::core::writeTextFileAtomic(building.fixture.path("roms/mame/" + name + ".zip"),
                                              rv::test::payloadBytes(9000 + static_cast<std::uint64_t>(index), 1024))
                    .has_value());
        fs::copy_file(cover, building.fixture.path("media/mame/covers/" + name + cover.extension().string()));
    }
    building.cli("import_again", {"import"});

    // The app's start: the stored plan shown, the building's update adds the sets, the notice over the lobby.
    const fs::path shot = building.dir / "notice-rooms-added.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "update",
                              {"--exit-after-frames", "--frames", "40", "--fixed-dt", "0.016667", "--screenshot",
                               arg(shot)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::WorldRecord& world = run.report->world;
    CHECK(world.outcome == "updated");
    CHECK(world.planWritten);
    CHECK(world.roomsAdded == std::vector<std::string>{"area:arcade:2"});
    CHECK(world.notice == "rooms-added");
    static_cast<void>(requireScreenshot(run, shot, kShotSize));
    const rv::world::StoredPlan after = building.plan();
    CHECK(placementsIn(after.plan, "area:arcade") + placementsIn(after.plan, "area:arcade:2") == 80);
    CHECK(placementsIn(after.plan, "area:arcade:2") > 0);
    CHECK(roomIn(after.plan, "area:arcade:2").expands.toString() == "area:arcade");
    // The next start: nothing added, no notice.
    const AppRun again = runApp(retroverseExe(), building.dir, "again",
                                {"--exit-after-frames", "--frames", "20", "--fixed-dt", "0.016667"});
    requireCleanRun(again);
    CHECK(again.report->world.roomsAdded.empty());
    CHECK(again.report->world.notice != "rooms-added");
}
