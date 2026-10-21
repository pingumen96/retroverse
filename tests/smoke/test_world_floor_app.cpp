// The floor in the app (I4 step 5a, ADR-0013 §7, §9): the building fixture (tests/fixtures/esde/building) recorded as
// the library of the case's data directory, imported and planned whole (building_support.hpp), then retroverse.exe
// without --scene:
//   - two rooms of one media family on a floor (derived from the plan: the handheld floor's cartridge rooms, each small
//     system in a console room of its own since I4 step 4 by the case's world.json, sharedRoomMinTitles 0,
//     building_support.hpp), started in
//     the first with --start-room: each room's collection samples its own texture arrays (the registry keys carry the
//     room id; the run report's residency.rooms), every room's covers resident, a static group per room, the floor's
//     GPU memory within the acceptable 512 MiB; the cover loader's first view the start room's items and those of the
//     rooms whose doorway it sees, none of another room's items the frustum meets beyond a wall; up close (--spawn at an
//     item's Focus pose, in the start room's frame) each room's fronts show their pictures and the reticle's item is
//     hovered in the room that holds the eye;
//   - a walk from the hall into the first room, back out, along the hall and into the second, where "Mostra nella
//     stanza" takes the walker to a title's item and it is inspected: the areas the eye crossed in order, the card of
//     the second room's game;
//   - the start in the ground floor's lobby without --start-room (no hero CRT bound), and a start room the building
//     lacks (exit 2);
//   - under the shipped threshold (I4 step 4, at its merge with step 5a): the arcade room and the handheld floor's
//     shared room from their doorways (--start-room area:arcade, area:shared:handheld), every item's cover resident,
//     their CRT the hero CRT, and up close a title of each section hovered with its cover;
//   - I4 step 6b: the hero CRT rebound on the walk from the first room to the second (after the dwell, the first's clip
//     never joined on the render loop, the frames of the rebind within the acceptable loading row in Release); a stored
//     floor previewed at the first frame from the plan and the items files, then reconciled room by room with nothing
//     moved or uploaded again; the building's additions (tests/fixtures/esde/building/additions) imported by "Aggiorna
//     libreria" while a floor shows: the handheld floor rebuilt around pokemini's new titles (a dressed level of the
//     shared room taken) with room:gb's pictures kept and the walker where it stood; gen4 rebuilt as megadrive enters
//     its new shared room, the walker kept in the hall's frame; gen5's new titles, only saved meanwhile, shown from the
//     saved plan and the items files at the next start.
// The screenshots stay in build/<preset>/smoke/world_floor_app for a look.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/cover_requests.hpp>
#include <rv/app/room_items.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/scene/camera.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/floor_layout.hpp>
#include <rv/world/floor_scene.hpp>
#include <rv/world/scene_builder.hpp>

#include "building_support.hpp"
#include "image_checks.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
constexpr float kEyeHeight = 1.65f;
/// The acceptable GPU memory of a floor (docs/roadmap.md, "Acceptable performance": all textures at most 512 MiB).
constexpr std::uint64_t kAcceptableGpuBytes = 512ull * 1024ull * 1024ull;
/// The walker's step at --fixed-dt 0.016667 (2.5 m/s) and its turn per mouse pixel.
constexpr double kStepMeters = 2.5 * 0.016667;
constexpr double kRadiansPerPixel = 0.0022;

/// A room of the floor as the tests use it: its id, its place, its collection and scene camera in its own frame.
struct FloorRoom {
    std::string id;
    rv::world::RoomPlacement placement;
    std::string family;
    rv::world::CollectionLayout layout;
    rv::scene::CameraSetup camera;
};

/// The rooms of `floor` in `plan` with their layouts (the run's assets).
std::vector<FloorRoom> floorRooms(const rv::world::WorldPlan& plan, std::string_view floor,
                                  const fs::path& assetRoot) {
    std::vector<FloorRoom> rooms;
    for (const rv::world::Room* room : rv::world::roomsOnFloor(plan, floor)) {
        auto data = rv::world::loadSceneData(assetRoot, plan, room->id);
        REQUIRE(data.has_value());
        auto layout = rv::world::collectionLayout(plan, room->id, *data, {});
        REQUIRE(layout.has_value());
        auto scene = rv::world::toSceneDescription(plan, room->id, *data);
        REQUIRE(scene.has_value());
        rooms.push_back(FloorRoom{room->id.toString(), room->placement, room->mediaFamily, std::move(*layout),
                                  scene->camera});
    }
    return rooms;
}

/// Two rooms of one media family on one floor of `plan`, both with items (the first floor that has them, by the plan's
/// floor order): their floor and ids, sorted.
struct RoomPair {
    std::string floor;
    std::string first;
    std::string second;
};

RoomPair sameFamilyRooms(const rv::world::WorldPlan& plan) {
    std::optional<RoomPair> pair;
    for (const rv::world::Floor& floor : plan.floors) {
        std::map<std::string, std::vector<std::string>> byFamily;
        for (const rv::world::Room* room : rv::world::roomsOnFloor(plan, floor.id)) {
            if (placementsIn(plan, room->id.toString()) > 0) {
                byFamily[room->mediaFamily].push_back(room->id.toString());
            }
        }
        for (const auto& [family, ids] : byFamily) {
            if (!pair && ids.size() >= 2) {
                pair = RoomPair{floor.id, ids[0], ids[1]};
            }
        }
    }
    INFO("a floor with two rooms of one media family");
    REQUIRE(pair.has_value());
    return *pair;
}

/// `placement` (a room's on its floor) seen from the room standing at `start` (app::relativePlacement's rule).
rv::world::RoomPlacement relativeTo(const rv::world::RoomPlacement& placement, const rv::world::RoomPlacement& start) {
    const std::int64_t sign = start.yawHalfTurns % 2 == 0 ? 1 : -1;
    return rv::world::RoomPlacement{sign * (placement.xMm - start.xMm), sign * (placement.zMm - start.zMm),
                                    (placement.yawHalfTurns + start.yawHalfTurns) % 2};
}

/// A point of the floor's frame (meters) in the frame of the room standing at `start`.
glm::vec3 inRoomFrame(const glm::vec3& point, const rv::world::RoomPlacement& start) {
    const float sign = start.yawHalfTurns % 2 == 0 ? 1.0f : -1.0f;
    return glm::vec3(sign * (point.x - static_cast<float>(start.xMm) / 1000.0f), point.y,
                     sign * (point.z - static_cast<float>(start.zMm) / 1000.0f));
}

/// The walker at the Focus pose of `item` (its layout in the start room's frame), and the camera it gives.
struct Viewpoint {
    rv::app::SpawnPoint spawn;
    rv::scene::CameraSetup camera;
};

Viewpoint focusViewpoint(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = 1280.0f / 720.0f;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
    Viewpoint viewpoint;
    viewpoint.spawn.feet = pose.feet;
    viewpoint.spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    viewpoint.spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
    viewpoint.camera = lens;
    viewpoint.camera.position = pose.feet + glm::vec3(0.0f, kEyeHeight, 0.0f);
    viewpoint.camera.yawDeg = *viewpoint.spawn.yawDeg;
    viewpoint.camera.pitchDeg = *viewpoint.spawn.pitchDeg;
    return viewpoint;
}

/// The middle of an item's visible face.
glm::vec3 faceOf(const rv::world::CollectionItem& item) {
    const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
    return glm::vec3(item.slotFrame * glm::vec4(local, 1.0f));
}

/// The spread of luma (its standard deviation) in the square of `radius` around `center`: a picture varies, the
/// pending flat label does not.
double lumaSpread(const rv::assets::ImageData& image, glm::ivec2 center, int radius) {
    double sum = 0.0;
    double squares = 0.0;
    int count = 0;
    for (int y = std::max(0, center.y - radius); y <= std::min(image.height - 1, center.y + radius); ++y) {
        for (int x = std::max(0, center.x - radius); x <= std::min(image.width - 1, center.x + radius); ++x) {
            const double luma = lumaAt(image, x, y);
            sum += luma;
            squares += luma * luma;
            ++count;
        }
    }
    if (count == 0) {
        return 0.0;
    }
    const double mean = sum / count;
    return std::sqrt(std::max(0.0, squares / count - mean * mean));
}

/// A lockstep run starting in `startRoom`: `frames` frames at a fixed step, a screenshot at the end when given.
std::vector<std::string> floorArguments(std::string_view startRoom, std::uint32_t frames,
                                        const std::optional<fs::path>& shot, std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--exit-after-frames", "--frames", std::to_string(frames), "--fixed-dt",
                                       "0.016667"};
    if (!startRoom.empty()) {
        arguments.insert(arguments.begin(), {"--start-room", std::string(startRoom)});
    }
    if (shot) {
        arguments.insert(arguments.end(), {"--screenshot", arg(*shot)});
    }
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// What every run on the floor must report: the floor shown from the first frame (lockstep), its rooms as the plan
/// places them, each collection room's layers resident in arrays of its own, the floor's GPU memory acceptable.
void requireFloorShown(const AppRun& run, const PlannedBuilding& building, std::string_view floor) {
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    const rv::app::WorldRecord& world = report.world;
    REQUIRE(world.bound);
    CHECK(world.state == "room");
    CHECK(world.floor == floor);
    CHECK(world.firstFrame == std::optional<std::uint64_t>(0));
    CHECK(world.hardFindings == 0);
    const rv::world::StoredPlan stored = building.plan();
    const std::vector<const rv::world::Room*> rooms = rv::world::roomsOnFloor(stored.plan, floor);
    REQUIRE(world.rooms.size() == rooms.size());
    std::size_t collections = 0;
    std::uint64_t items = 0;
    for (std::size_t index = 0; index < rooms.size(); ++index) {
        const rv::world::Room& room = *rooms[index];
        const rv::app::WorldRoomRecord& record = world.rooms[index];
        CHECK(record.id == room.id.toString());
        CHECK(record.floor == room.floor);
        CHECK(record.xMm == room.placement.xMm);
        CHECK(record.zMm == room.placement.zMm);
        CHECK(record.yawHalfTurns == room.placement.yawHalfTurns);
        const std::size_t placed = placementsIn(stored.plan, record.id);
        collections += placed > 0 ? 1 : 0;
        items += placed;
    }
    CHECK(world.items == items);
    // Residency per room: every room's layers resident, in arrays its own materials sample (ADR-0013 §9).
    const rv::app::ResidencyRecord& residency = report.residency;
    REQUIRE(residency.bound);
    CHECK(residency.items == items);
    CHECK(residency.frontsResident == items);
    CHECK(residency.spinesResident == items);
    CHECK(residency.pending == 0);
    REQUIRE(residency.rooms.size() == collections);
    std::uint64_t arrayBytes = 0;
    std::uint64_t roomItems = 0;
    for (const rv::app::RoomResidencyRecord& room : residency.rooms) {
        INFO(room.room);
        CHECK(room.items == placementsIn(stored.plan, room.room));
        CHECK(room.frontsResident == room.items);
        CHECK(room.spinesResident == room.items);
        CHECK(room.pending == 0);
        CHECK(room.arrays >= 2);
        CHECK(room.ownArrays == room.arrays);
        arrayBytes += room.arrayBytes;
        roomItems += room.items;
    }
    CHECK(arrayBytes == residency.arrayBytes);
    CHECK(roomItems == residency.items);
    // A static group per collection room.
    REQUIRE(report.lastView.has_value());
    CHECK(report.lastView->staticGroups == collections);
    // The floor within the acceptable GPU memory.
    INFO("GPU memory peak " << report.gpuMemory.peakTotalBytes / (1024 * 1024) << " MiB");
    CHECK(report.gpuMemory.peakTotalBytes <= kAcceptableGpuBytes);
}

/// The items of a room's layout of `section` whose face projects into the view of `camera`, and how many of them show
/// a picture (luma spread in the square of `radius` pixels at their face's middle).
std::pair<std::size_t, std::size_t> texturedFronts(const rv::assets::ImageData& image,
                                                   const rv::scene::CameraSetup& camera,
                                                   const rv::world::CollectionLayout& layout,
                                                   rv::world::Section section, int radius = 8) {
    std::size_t probed = 0;
    std::size_t textured = 0;
    for (const rv::world::CollectionItem& item : layout.items) {
        if (item.section != section) {
            continue;
        }
        const std::optional<glm::ivec2> pixel = projectToPixel(camera, kShotSize, faceOf(item));
        if (!pixel || pixel->x < 16 || pixel->y < 16 || pixel->x >= kShotSize.x - 16 || pixel->y >= kShotSize.y - 16) {
            continue;
        }
        ++probed;
        const double spread = lumaSpread(image, *pixel, radius);
        if (spread < 6.0) {
            // Said with the caller's next assertion.
            UNSCOPED_INFO("item " << item.game.toString() << " at (" << pixel->x << ", " << pixel->y
                                  << "): luma spread " << spread);
        }
        textured += spread >= 6.0 ? 1 : 0;
    }
    return {probed, textured};
}

const FloorRoom& roomNamed(const std::vector<FloorRoom>& rooms, std::string_view id) {
    const auto found = std::find_if(rooms.begin(), rooms.end(), [&](const FloorRoom& room) { return room.id == id; });
    INFO(id);
    REQUIRE(found != rooms.end());
    return *found;
}

/// The first face-out item of `layout`.
const rv::world::CollectionItem& firstFaceOut(const rv::world::CollectionLayout& layout) {
    const auto found = std::find_if(layout.items.begin(), layout.items.end(), [](const rv::world::CollectionItem& item) {
        return item.stance == rv::world::ItemStance::FaceOut;
    });
    REQUIRE(found != layout.items.end());
    return *found;
}

/// The scene build of `floor` as the app makes it (its hall, its scene file's camera; the run's assets).
rv::world::FloorSceneBuild floorSceneOf(const rv::world::WorldPlan& plan, std::string_view floor,
                                        const fs::path& assetRoot) {
    auto data = rv::world::loadFloorSceneData(assetRoot, plan, floor);
    REQUIRE(data.has_value());
    auto build = rv::world::toFloorSceneFile(plan, floor, *data);
    REQUIRE(build.has_value());
    return std::move(*build);
}

float meters(std::int64_t millimetres) {
    return static_cast<float>(millimetres) / 1000.0f;
}

/// The z of the hall's centre line, in the floor's frame.
float hallCentreZ(const rv::world::Hall& hall) {
    return meters(hall.interior.minZMm + hall.interior.maxZMm) / 2.0f;
}

/// The hall's doorway of `room` (which must have one onto the hall).
const rv::world::FloorDoorway& hallDoorwayOf(const rv::world::Hall& hall, std::string_view room) {
    const auto found = std::find_if(hall.doorways.begin(), hall.doorways.end(),
                                    [&](const rv::world::FloorDoorway& doorway) { return doorway.room.toString() == room; });
    INFO(room);
    REQUIRE(found != hall.doorways.end());
    REQUIRE(found->opensOntoHall());
    return *found;
}

/// The doorway of a room on its floor, in the floor's frame (derived from the hall, floor_layout.hpp): the x of its
/// passage's middle, the direction into the room from the hall (-1 for a north-row room, +1 for a south-row one) and
/// how far the clear area inside it lies from the hall's centre line.
struct Doorway {
    float x = 0.0f;
    float into = -1.0f;
    float depth = 0.0f;
};

Doorway doorwayOf(const rv::world::Hall& hall, std::string_view room) {
    const rv::world::FloorDoorway& doorway = hallDoorwayOf(hall, room);
    const float centre = hallCentreZ(hall);
    const float inside = meters(doorway.clearCentre.zMm);
    return Doorway{meters(doorway.passage.minXMm + doorway.passage.maxXMm) / 2.0f, inside < centre ? -1.0f : 1.0f,
                   std::abs(inside - centre)};
}

/// The passage of `room`'s doorway onto the hall as a box from the floor to the doorway's top, in the frame of the room
/// standing at `start` (the app's doorway box, the cover loader's view).
rv::core::Aabb doorwayBoxIn(const rv::world::Hall& hall, std::string_view room,
                            const rv::world::RoomPlacement& start) {
    const rv::world::FloorDoorway& doorway = hallDoorwayOf(hall, room);
    const glm::vec3 a = inRoomFrame(glm::vec3(meters(doorway.passage.minXMm), 0.0f, meters(doorway.passage.minZMm)), start);
    const glm::vec3 b = inRoomFrame(
        glm::vec3(meters(doorway.passage.maxXMm), meters(doorway.heightMm), meters(doorway.passage.maxZMm)), start);
    return rv::core::Aabb{glm::min(a, b), glm::max(a, b)};
}

/// An input script's turn of `degrees` (positive: right) over `frames` frames from `from`.
Json turn(std::uint64_t from, std::uint64_t frames, double degrees) {
    const double pixels = rv::core::degreesToRadians(static_cast<float>(degrees)) / kRadiansPerPixel /
                          static_cast<double>(frames);
    return Json{{"fromFrame", from}, {"toFrame", from + frames - 1}, {"actions", {{"look_delta_x", pixels}}}};
}

/// An input script's walk of `meters` forward from `from`; the frames it takes.
std::uint64_t walkFrames(double meters) {
    return static_cast<std::uint64_t>(std::llround(std::abs(meters) / kStepMeters));
}

Json walk(std::uint64_t from, std::uint64_t frames) {
    return Json{{"fromFrame", from}, {"toFrame", from + frames - 1}, {"actions", {{"move_forward", 1.0}}}};
}

/// A room id as a file name's part ("room:gb" gives "room_gb").
std::string fileStem(std::string id) {
    std::replace(id.begin(), id.end(), ':', '_');
    return id;
}

/// `degrees` within (-180, 180].
double wrapped(double degrees) {
    while (degrees > 180.0) {
        degrees -= 360.0;
    }
    while (degrees <= -180.0) {
        degrees += 360.0;
    }
    return degrees;
}

} // namespace

TEST_CASE("world floor app: two rooms of one media family each sample their own arrays and show their own covers",
          "[gpu][world][world_floor_app]") {
    const PlannedBuilding building("world_floor_app/two_rooms");
    const rv::world::StoredPlan stored = building.plan();
    const RoomPair pair = sameFamilyRooms(stored.plan);
    INFO("the floor " << pair.floor << ": " << pair.first << " and " << pair.second);

    const fs::path doorway = building.dir / "doorway.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "doorway", floorArguments(pair.first, 40, doorway));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    requireFloorShown(run, building, pair.floor);
    CHECK(run.report->world.room == pair.first);
    REQUIRE(run.report->sceneAreas.has_value());
    CHECK(run.report->sceneAreas->area == pair.first);
    // The hero CRT is the start room's (I4 step 5a), the room that holds the eye (I4 step 6b).
    CHECK(run.report->video.screen.starts_with(pair.first + "/"));
    CHECK(run.report->world.screenRoom == pair.first);
    (void)requireScreenshot(run, doorway, kShotSize);

    // Up close, in the start room's frame: each room's fronts show their pictures, and the reticle's item is hovered in
    // the room that holds the eye (a pick index per room).
    const std::vector<FloorRoom> rooms = floorRooms(stored.plan, pair.floor, run.report->assetRoot);
    const FloorRoom& start = roomNamed(rooms, pair.first);

    // The cover loader's first view (ADR-0013 §9): the start room's items, and those of a room whose doorway the frustum
    // meets; a room whose doorway it does not see has none in view, even with items in the frustum beyond a wall (they
    // come by distance). Seen from the doorway camera, then turned (from it, in steps of 5 degrees) until the frustum
    // meets items of a room whose doorway it misses, so that the rule decides something.
    {
        const rv::world::FloorSceneBuild floorScene = floorSceneOf(stored.plan, pair.floor, run.report->assetRoot);
        // The rooms whose doorway the camera at `yawDeg` misses (a frustum a little wider than the app's, 16:9 like the
        // window and the screenshot, so that "missed" holds for the app's too), with their items whose middle it shows
        // well inside the picture.
        const auto unseenAt = [&](float yawDeg) {
            rv::scene::CameraSetup setup = start.camera;
            setup.yawDeg = yawDeg;
            rv::scene::CameraLens lens = setup.lens;
            lens.horizontalFovDeg += 2.0f;
            lens.farPlane = std::max(lens.farPlane, 500.0f);
            rv::scene::Camera wider;
            wider.setLens(lens);
            wider.setAspect(16.0f / 9.0f);
            wider.setPose(setup.position, rv::core::degreesToRadians(yawDeg),
                          rv::core::degreesToRadians(setup.pitchDeg));
            const rv::app::CoverView widerView = rv::app::CoverView::of(wider);
            std::map<std::string, std::size_t> unseen;
            for (const FloorRoom& room : rooms) {
                if (room.id == pair.first || placementsIn(stored.plan, room.id) == 0 ||
                    rv::app::coverInView(doorwayBoxIn(floorScene.hall, room.id, start.placement), widerView)) {
                    continue;
                }
                const rv::world::CollectionLayout layout =
                    rv::world::floorCollectionLayout(room.layout, relativeTo(room.placement, start.placement));
                std::size_t shown = 0;
                for (const rv::world::CollectionItem& item : layout.items) {
                    const std::optional<glm::ivec2> pixel = projectToPixel(setup, kShotSize, faceOf(item));
                    shown += pixel && pixel->x >= 64 && pixel->y >= 64 && pixel->x < kShotSize.x - 64 &&
                                     pixel->y < kShotSize.y - 64
                                 ? 1
                                 : 0;
                }
                unseen.emplace(room.id, shown);
            }
            return unseen;
        };
        const auto viewItemsOf = [](const rv::app::RunReport& report) {
            std::map<std::string, std::uint64_t> items;
            for (const rv::app::RoomResidencyRecord& room : report.residency.rooms) {
                items[room.room] = room.viewItems;
            }
            return items;
        };
        const std::map<std::string, std::uint64_t> atDoorway = viewItemsOf(*run.report);
        CHECK(atDoorway.at(pair.first) > 0);
        CHECK(atDoorway.at(pair.first) <= placementsIn(stored.plan, pair.first));
        for (const auto& [room, shown] : unseenAt(start.camera.yawDeg)) {
            INFO(room << ", " << shown << " item(s) in the frustum");
            CHECK(atDoorway.at(room) == 0);
        }
        std::optional<float> turned;
        for (int step = 1; step < 72 && !turned; ++step) {
            const float yaw = static_cast<float>(wrapped(start.camera.yawDeg + 5.0 * step));
            for (const auto& [room, shown] : unseenAt(yaw)) {
                turned = shown > 0 ? std::optional<float>(yaw) : turned;
            }
        }
        INFO("a turn from the doorway camera that meets items of a room whose doorway it misses");
        REQUIRE(turned.has_value());
        const rv::app::SpawnPoint pose{start.camera.position - glm::vec3(0.0f, kEyeHeight, 0.0f), *turned,
                                       start.camera.pitchDeg};
        const AppRun turnedRun = runApp(retroverseExe(), building.dir, "first_view",
                                        floorArguments(pair.first, 2, std::nullopt, {"--spawn", spawnArgument(pose)}));
        requireCleanRun(turnedRun);
        INFO(turnedRun.describe());
        CHECK(turnedRun.report->world.room == pair.first);
        const std::map<std::string, std::uint64_t> atTurn = viewItemsOf(*turnedRun.report);
        std::size_t beyondWalls = 0;
        for (const auto& [room, shown] : unseenAt(*turned)) {
            INFO("yaw " << *turned << ": " << room << ", " << shown << " item(s) in the frustum");
            CHECK(atTurn.at(room) == 0);
            beyondWalls += shown;
        }
        CHECK(beyondWalls > 0);
    }

    for (const std::string& id : {pair.first, pair.second}) {
        INFO(id);
        const FloorRoom& room = roomNamed(rooms, id);
        const rv::world::CollectionLayout layout =
            rv::world::floorCollectionLayout(room.layout, relativeTo(room.placement, start.placement));
        const rv::world::CollectionItem& item = firstFaceOut(layout);
        const Viewpoint close = focusViewpoint(item, start.camera);
        const fs::path shot = building.dir / (fileStem(id) + "-close.png");
        const AppRun closeRun =
            runApp(retroverseExe(), building.dir, fileStem(id) + "_close",
                   floorArguments(pair.first, 12, shot, {"--spawn", spawnArgument(close.spawn)}));
        requireCleanRun(closeRun);
        INFO(closeRun.describe());
        CHECK(closeRun.report->world.room == id);
        CHECK(closeRun.report->world.hoveredGame == item.game.toString());
        CHECK(closeRun.report->world.hoverChanges >= 1);
        const rv::assets::ImageData image = requireScreenshot(closeRun, shot, kShotSize);
        const auto [probed, textured] = texturedFronts(image, close.camera, layout, item.section);
        CHECK(textured == probed); // first: it carries the probes that found no picture
        CHECK(probed >= 2);
    }
}

TEST_CASE("world floor app: a walk from the hall into one room, then into another, a cover inspected there",
          "[gpu][world][world_floor_app]") {
    const PlannedBuilding building("world_floor_app/walk");
    const rv::world::StoredPlan stored = building.plan();
    const RoomPair pair = sameFamilyRooms(stored.plan);
    // A first run writes the rooms' items (the titles the search below types).
    const AppRun first = runApp(retroverseExe(), building.dir, "first", floorArguments(pair.first, 2, std::nullopt));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const std::vector<FloorRoom> rooms = floorRooms(stored.plan, pair.floor, first.report->assetRoot);
    const FloorRoom& roomA = roomNamed(rooms, pair.first);
    const FloorRoom& roomB = roomNamed(rooms, pair.second);
    // The game to inspect in the second room: its first face-out item, whose title the quick catalog's search finds
    // first (its full title, unique in the fixture).
    const rv::world::CollectionItem& target = firstFaceOut(roomB.layout);
    const rv::world::Room& planB = roomIn(stored.plan, pair.second);
    REQUIRE_FALSE(planB.sections.empty());
    const auto items = rv::app::readRoomItems(rv::app::roomItemsFile(
        building.data / "cache", rv::catalog::LibraryId::parse("esde").value(), planB.sections.front()));
    REQUIRE(items.has_value());
    const rv::app::RoomItemText* text = items->find(target.game);
    REQUIRE(text != nullptr);
    REQUIRE_FALSE(text->title.empty());
    std::string search = text->title;
    std::transform(search.begin(), search.end(), search.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });

    // The walk, in the floor's frame (the spawn then moved into the first room's frame, the start room's), derived from
    // the floor's hall (floor_layout.hpp): from the hall's centre line in front of the first room's doorway through its
    // passage to the clear area inside it, back out, along the hall to the second room's doorway and into it.
    const rv::world::FloorSceneBuild floorScene = floorSceneOf(stored.plan, pair.floor, first.report->assetRoot);
    const Doorway doorA = doorwayOf(floorScene.hall, pair.first);
    const Doorway doorB = doorwayOf(floorScene.hall, pair.second);
    INFO("doorways at x " << doorA.x << " (" << doorA.depth << " m deep) and " << doorB.x << " (" << doorB.depth
                          << " m deep)");
    const double yawIntoA = doorA.into < 0.0f ? 0.0 : 180.0;
    const double yawIntoB = doorB.into < 0.0f ? 0.0 : 180.0;
    const double alongHall = doorB.x >= doorA.x ? 90.0 : -90.0;
    Json script = Json::array();
    std::uint64_t frame = 0;
    const std::uint64_t intoA = walkFrames(doorA.depth);
    script.push_back(walk(frame, intoA)); // into the first room
    frame += intoA;
    script.push_back(turn(frame, 20, 180.0));
    frame += 20;
    script.push_back(walk(frame, intoA)); // back to the hall's centre line
    frame += intoA;
    double yaw = wrapped(yawIntoA + 180.0);
    const std::uint64_t along = walkFrames(doorB.x - doorA.x);
    if (along > 0) {
        script.push_back(turn(frame, 10, wrapped(alongHall - yaw)));
        frame += 10;
        yaw = alongHall;
        script.push_back(walk(frame, along));
        frame += along;
    }
    const double toB = wrapped(yawIntoB - yaw);
    if (std::abs(toB) > 0.01) {
        const std::uint64_t frames = std::abs(toB) > 90.5 ? 20 : 10;
        script.push_back(turn(frame, frames, toB));
        frame += frames;
    }
    const std::uint64_t intoB = walkFrames(doorB.depth);
    script.push_back(walk(frame, intoB)); // into the second room
    frame += intoB;
    const std::uint64_t entered = frame;
    // "Mostra nella stanza" (world_show_in_room.json's path): the quick catalog, the search field, the title typed, the
    // list, the card's actions, the button; the walker eased to the item's Focus pose in 600 ms; then interact: Inspect.
    frame += 5;
    script.push_back(Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{"quick_catalog", 1}}}});
    script.push_back(Json{{"fromFrame", frame + 3}, {"toFrame", frame + 3}, {"actions", {{"ui_up", 1}}}});
    script.push_back(Json{{"fromFrame", frame + 6}, {"toFrame", frame + 6}, {"actions", {{"ui_up", 1}}}});
    script.push_back(Json{{"fromFrame", frame + 9}, {"toFrame", frame + 9}, {"text", search}});
    script.push_back(Json{{"fromFrame", frame + 12}, {"toFrame", frame + 12}, {"actions", {{"ui_activate", 1}}}});
    script.push_back(Json{{"fromFrame", frame + 15}, {"toFrame", frame + 15}, {"actions", {{"ui_right", 1}}}});
    script.push_back(Json{{"fromFrame", frame + 18}, {"toFrame", frame + 18}, {"actions", {{"ui_down", 1}}}});
    script.push_back(Json{{"fromFrame", frame + 21}, {"toFrame", frame + 21}, {"actions", {{"ui_activate", 1}}}});
    frame += 21 + 60;
    script.push_back(Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{"interact", 1}}}});
    const std::uint64_t frames = frame + 40;
    const fs::path scriptFile = building.dir / "walk.json";
    REQUIRE(rv::core::writeTextFileAtomic(scriptFile, script.dump(2) + "\n").has_value());

    // The spawn: the hall's centre line in front of the first room's doorway, facing it, in the first room's frame.
    const glm::vec3 hallFeet = inRoomFrame(glm::vec3(doorA.x, 0.0f, hallCentreZ(floorScene.hall)), roomA.placement);
    const double startYaw = wrapped(yawIntoA + (roomA.placement.yawHalfTurns % 2 == 0 ? 0.0 : 180.0));
    const rv::app::SpawnPoint spawn{hallFeet, static_cast<float>(startYaw), 0.0f};
    const fs::path shot = building.dir / "inspected.png";
    const AppRun run =
        runApp(retroverseExe(), building.dir, "walk",
               floorArguments(pair.first, static_cast<std::uint32_t>(frames), shot,
                              {"--spawn", spawnArgument(spawn), "--input-script", arg(scriptFile)}));
    requireCleanRun(run);
    requireFloorShown(run, building, pair.floor);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    // The areas the eye crossed, in order: the hall, the first room, the hall, the second room.
    REQUIRE(report.sceneAreas.has_value());
    std::vector<std::string> areas;
    for (const rv::app::SceneAreaChange& change : report.sceneAreas->changes) {
        areas.push_back(change.area);
    }
    const std::string hall = "hall:" + pair.floor;
    CHECK(areas == std::vector<std::string>{hall, pair.first, hall, pair.second});
    CHECK(report.sceneAreas->changes.back().frame <= entered);
    CHECK(report.world.room == pair.second);
    // The second room's game shown in its room, then inspected: its card.
    REQUIRE(report.catalog.has_value());
    CHECK(report.catalog->selectedId == target.game.toString());
    CHECK(report.world.launch.showInRoom == 1);
    CHECK(report.world.launch.showInRoomFailed == 0);
    CHECK(report.world.interaction.state == "inspect");
    CHECK(report.world.interaction.focusedGame == target.game.toString());
    CHECK(report.world.card.game == target.game.toString());
    CHECK(report.world.card.coverFrames > 0);
    (void)requireScreenshot(run, shot, kShotSize);

    // I4 step 6b (ADR-0013 §9): the hero CRT follows the room that holds the eye: the first room's from the start (its
    // clip playing), the second's once the eye has stayed in it for the dwell (0.3 s: 18 frames), never in the hall or
    // on the way back through the first. The first room's clip stops as a clip replacement does: its decoder's thread
    // ends on its own, never joined on the render loop, and never two decoder threads at once.
    CHECK(report.world.screenRoom == pair.second);
    REQUIRE(report.video.bound);
    CHECK(report.video.screen.starts_with(pair.second + "/"));
    CHECK(report.video.decodersStarted >= 1);
    CHECK(report.video.maxDecoderThreads <= 1);
    CHECK(report.video.joinedAtExit);
    REQUIRE(report.video.binds.size() == 1);
    const rv::app::VideoBindRecord& bind = report.video.binds.front();
    CHECK(bind.screen.starts_with(pair.second + "/"));
    const std::uint64_t enteredB = report.sceneAreas->changes.back().frame;
    CHECK(bind.frame >= enteredB + 18);
    CHECK(bind.frame <= enteredB + 20);
    CHECK(bind.bindMs < 50.0);
    const auto bindPhase = std::find_if(report.phases.begin(), report.phases.end(),
                                        [](const rv::app::PhaseRecord& phase) { return phase.name == "screenBind"; });
    REQUIRE(bindPhase != report.phases.end());
    CHECK(bindPhase->opened == 1);
    CHECK(bindPhase->frames >= 60);
    REQUIRE(bindPhase->times.has_value());
    WARN("the hero CRT's rebind: " << bind.bindMs << " ms on the main thread, " << bind.threadsRunning
                                    << " decoder thread(s) still ending; its frames p99 " << bindPhase->times->p99Ms
                                    << " ms, max " << bindPhase->times->maxMs << " ms");
#if defined(NDEBUG)
    // The acceptable loading row (docs/roadmap.md): frames while loading at most 50 ms at the 99th percentile.
    CHECK(bindPhase->times->p99Ms <= 50.0);
#endif
}

TEST_CASE("world floor app: without --start-room the run starts in the ground floor's lobby; a room the building lacks "
          "ends the run",
          "[gpu][world][world_floor_app]") {
    const PlannedBuilding building("world_floor_app/lobby");
    const fs::path shot = building.dir / "lobby.png";
    const AppRun run = runApp(retroverseExe(), building.dir, "lobby", floorArguments({}, 40, shot));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    requireFloorShown(run, building, "ground");
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    // In the lobby, at the elevator's arrival point, facing east (the floor scene's camera, floor_layout.hpp): no room
    // holds the eye, no hero CRT is bound.
    CHECK(report.world.room.empty());
    REQUIRE(report.sceneAreas.has_value());
    CHECK(report.sceneAreas->area == rv::world::hallAreaId(rv::world::kGroundFloor));
    CHECK_FALSE(report.video.bound);
    const rv::world::FloorSceneBuild lobby =
        floorSceneOf(building.plan().plan, rv::world::kGroundFloor, report.assetRoot);
    const Json& camera = lobby.sceneFile.at("camera");
    REQUIRE(report.finalCamera.has_value());
    CHECK(std::abs(report.finalCamera->feet.x - meters(lobby.hall.arrival.xMm)) < 1e-3f);
    CHECK(std::abs(report.finalCamera->feet.z - meters(lobby.hall.arrival.zMm)) < 1e-3f);
    CHECK(std::abs(report.finalCamera->feet.x - camera.at("position")[0].get<float>()) < 1e-3f);
    CHECK(std::abs(report.finalCamera->feet.z - camera.at("position")[2].get<float>()) < 1e-3f);
    CHECK(std::abs(report.finalCamera->yawDeg.value_or(0.0f) - camera.value("yawDeg", 0.0f)) < 1e-3f);
    // East is the floor frame's +X (building.hpp): the walker's forward (yaw 0 looks at -Z, positive turns right).
    const float yaw = rv::core::degreesToRadians(report.finalCamera->yawDeg.value_or(0.0f));
    CHECK(std::sin(yaw) > 0.999f);
    const rv::assets::ImageData image = requireScreenshot(run, shot, kShotSize);
    CHECK(lumaSummary(image).mean > 10.0);

    // A start room the building lacks: the run ends with exit code 2 once the plan is known.
    const AppRun missing =
        runApp(retroverseExe(), building.dir, "missing", floorArguments("room:megadrive", 5, std::nullopt));
    INFO(missing.describe());
    CHECK(missing.exitCode == 2);
    REQUIRE(missing.report.has_value());
    CHECK(missing.report->exitCode == 2);
    CHECK(missing.report->failure.value_or(std::string()).find("room:megadrive") != std::string::npos);
}

TEST_CASE("world floor app: the arcade room and a floor's shared room show their sections' covers",
          "[gpu][world][world_floor_app]") {
    // The building under the shipped threshold (30 distinct titles, no world.json): mame in the arcade room, pocketlab
    // (6 titles) and pokemini (8) in the sections of the handheld floor's shared room (I4 step 4).
    const PlannedBuilding building("world_floor_app/shared", false);
    const rv::world::StoredPlan stored = building.plan();
    CHECK(building.roomOf("mame") == "area:arcade");
    CHECK(building.roomOf("pocketlab") == "area:shared:handheld");
    CHECK(building.roomOf("pokemini") == "area:shared:handheld");
    const rv::catalog::LibraryId library = rv::catalog::LibraryId::parse("esde").value();
    for (const std::string id : {"area:shared:handheld", "area:arcade"}) {
        INFO(id);
        const rv::world::Room& planned = roomIn(stored.plan, id);
        CHECK(planned.mediaFamily == "generic_case");
        const fs::path doorway = building.dir / (fileStem(id) + "-doorway.png");
        const AppRun run =
            runApp(retroverseExe(), building.dir, fileStem(id) + "_doorway", floorArguments(id, 40, doorway));
        skipWithoutOpenGl(run);
        requireCleanRun(run);
        requireFloorShown(run, building, planned.floor);
        INFO(run.describe());
        const rv::app::RunReport& report = *run.report;
        CHECK(report.world.room == id);
        // Every item of the floor shows its cover (every placed game of these systems has one).
        CHECK(report.residency.covers == report.residency.items);
        CHECK(report.residency.labels == 0);
        CHECK(report.residency.coverFailures == 0);
        // The room's CRT is the hero CRT (mame's one clip playing in the arcade room; the shared room's sections have
        // no clip: its colour bars), not another room's of the floor (the handheld floor's room:gb has a clip), with
        // every section's placed games.
        CHECK(report.video.bound);
        CHECK(report.video.screen == id + "/fx:hero.crt");
        if (id == "area:arcade") {
            CHECK(placementsIn(stored.plan, id) == 15);
            CHECK(report.world.screenGames == 15);
            const auto played = rv::catalog::GameId::parse(report.video.gameId);
            REQUIRE(played.has_value());
            CHECK(std::any_of(stored.plan.placements.begin(), stored.plan.placements.end(),
                              [&](const rv::world::Placement& placement) {
                                  return placement.game == *played &&
                                         placement.slot.fixture().room().toString() == id;
                              }));
        } else {
            CHECK(placementsIn(stored.plan, id) == 14);
            CHECK(report.world.screenGames == 14); // both sections' games: pocketlab's 6, pokemini's 8
            CHECK(report.video.gameId.empty());
            CHECK(report.video.picture == "idle-pattern");
        }
        (void)requireScreenshot(run, doorway, kShotSize);

        // Up close, a face-out title of each section (the sections' games told apart by their items files: each holds
        // the text of its own games): hovered in the room, its cover its hero picture, the fronts around it pictures.
        const std::vector<FloorRoom> rooms = floorRooms(stored.plan, planned.floor, report.assetRoot);
        const FloorRoom& room = roomNamed(rooms, id);
        for (const rv::catalog::SystemName& section : planned.sections) {
            INFO(section.str());
            const auto items =
                rv::app::readRoomItems(rv::app::roomItemsFile(building.data / "cache", library, section));
            REQUIRE(items.has_value());
            const auto found =
                std::find_if(room.layout.items.begin(), room.layout.items.end(), [&](const rv::world::CollectionItem& item) {
                    const rv::app::RoomItemText* text = items->find(item.game);
                    return item.stance == rv::world::ItemStance::FaceOut && text != nullptr && !text->coverPath.empty();
                });
            REQUIRE(found != room.layout.items.end());
            const Viewpoint close = focusViewpoint(*found, room.camera);
            const std::string name = fileStem(id) + "_" + section.str() + "_close";
            const fs::path shot = building.dir / (name + ".png");
            const AppRun closeRun = runApp(retroverseExe(), building.dir, name,
                                           floorArguments(id, 12, shot, {"--spawn", spawnArgument(close.spawn)}));
            requireCleanRun(closeRun);
            INFO(closeRun.describe());
            CHECK(closeRun.report->world.room == id);
            CHECK(closeRun.report->world.hoveredGame == found->game.toString());
            CHECK(closeRun.report->residency.hero.shownGame == found->game.toString());
            CHECK(closeRun.report->residency.hero.failed == 0);
            const rv::assets::ImageData image = requireScreenshot(closeRun, shot, kShotSize);
            // A generic case's front is about twice a cartridge's on screen, and the fixture's pictures' stripes as
            // wide: a wider probe than the cartridges' (a flat stripe is no flat label).
            const auto [probed, textured] = texturedFronts(image, close.camera, room.layout, found->section, 24);
            CHECK(textured == probed); // first: it carries the probes that found no picture
            CHECK(probed >= 1);
        }
    }
}

namespace {

/// "Aggiorna libreria" pressed while the floor shows (tests/input_scripts/catalog_refresh.json's path): the quick
/// catalog opens on frame 2, Home and Up four times reach the button, A presses it on frame 20, the quick catalog closes
/// on frame 24.
fs::path aggiornaScript(const fs::path& dir) {
    const Json script = Json::array({{{"fromFrame", 2}, {"toFrame", 2}, {"actions", {{"quick_catalog", 1}}}},
                                     {{"fromFrame", 4}, {"toFrame", 4}, {"actions", {{"ui_home", 1}}}},
                                     {{"fromFrame", 6}, {"toFrame", 6}, {"actions", {{"ui_up", 1}}}},
                                     {{"fromFrame", 8}, {"toFrame", 8}, {"actions", {{"ui_up", 1}}}},
                                     {{"fromFrame", 10}, {"toFrame", 10}, {"actions", {{"ui_up", 1}}}},
                                     {{"fromFrame", 12}, {"toFrame", 12}, {"actions", {{"ui_up", 1}}}},
                                     {{"fromFrame", 20}, {"toFrame", 20}, {"actions", {{"ui_activate", 1}}}},
                                     {{"fromFrame", 24}, {"toFrame", 24}, {"actions", {{"quick_catalog", 1}}}}});
    const fs::path file = dir / "aggiorna.json";
    REQUIRE(rv::core::writeTextFileAtomic(file, script.dump(2) + "\n").has_value());
    return file;
}

/// The part of the room `room` in a reconciliation's or a live update's record (REQUIREd).
const rv::app::RoomUpdateRecord& roomUpdate(const std::vector<rv::app::RoomUpdateRecord>& rooms, std::string_view room) {
    const auto found = std::find_if(rooms.begin(), rooms.end(),
                                    [&](const rv::app::RoomUpdateRecord& record) { return record.room == room; });
    INFO("the room " << room);
    REQUIRE(found != rooms.end());
    return *found;
}

/// Every collection room of the run's floor: nothing uploaded over a picture its item showed already.
void requireNothingUploadedAgain(const rv::app::RunReport& report) {
    for (const rv::app::RoomResidencyRecord& room : report.residency.rooms) {
        INFO(room.room << ": " << room.uploads << " layer(s) uploaded");
        CHECK(room.reuploads == 0);
    }
}

} // namespace

TEST_CASE("world floor app: a stored floor previewed, then reconciled room by room with nothing moved or uploaded "
          "again; the building's additions refreshed while it shows: rebuilt, the rooms' pictures and the walker kept",
          "[gpu][world][world_floor_app]") {
    // The handheld floor under the shipped threshold (I4 step 4): room:gb and the shared room of pocketlab and pokemini.
    const PlannedBuilding building("world_floor_app/live_handheld", false);
    const std::string gb = building.roomOf("gb");
    const std::string shared = building.roomOf("pokemini");
    CHECK(gb == "room:gb");
    CHECK(shared == "area:shared:handheld");
    // A first run keeps the rooms' items for the next start.
    const AppRun first = runApp(retroverseExe(), building.dir, "first", floorArguments(gb, 2, std::nullopt));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    CHECK(first.report->world.itemsWritten);

    // The next start (ADR-0013 §9): the stored floor at the first frame from the plan and the items files, its rooms'
    // CRT games among them; then the building's result reconciled room by room (I4 step 6b): each room's content
    // digest as stored, its items in place, its covers served again only to check their sources (the media root known
    // now), nothing moved, nothing uploaded over a picture that showed, nothing written.
    const AppRun second = runApp(retroverseExe(), building.dir, "second", floorArguments(gb, 40, std::nullopt));
    requireCleanRun(second);
    requireFloorShown(second, building, "handheld");
    {
        INFO(second.describe());
        const rv::app::WorldRecord& world = second.report->world;
        CHECK(world.outcome == "unchanged");
        CHECK_FALSE(world.planWritten);
        CHECK_FALSE(world.itemsWritten);
        REQUIRE(world.preview.has_value());
        CHECK(world.preview->frame == 0);
        CHECK(world.preview->items == world.items);
        CHECK(world.preview->itemsKnown == world.items);
        REQUIRE(world.reconcile.has_value());
        CHECK(world.reconcile->kind == "textures");
        CHECK(world.reconcile->frame == 1);
        CHECK(world.reconcile->moved == 0);
        CHECK(world.reconcile->added == 0);
        CHECK(world.reconcile->removed == 0);
        CHECK(world.reconcile->kept == world.items);
        REQUIRE(world.reconcile->rooms.size() == 2);
        for (const rv::app::RoomUpdateRecord& room : world.reconcile->rooms) {
            INFO(room.room);
            CHECK(room.kind == "textures");
            CHECK(room.moved == 0);
            CHECK(room.added == 0);
            CHECK(room.coversChanged == 0);
        }
        CHECK_FALSE(world.reconcile->walker.has_value());
        requireNothingUploadedAgain(*second.report);
        CHECK(world.screenRoom == gb);
        CHECK(second.report->video.screen.starts_with(gb + "/"));
        CHECK(second.report->video.binds.empty());
    }

    // The additions laid over the collection, imported by "Aggiorna libreria" while the floor shows: pokemini's titles
    // take a dressed level of the shared room (its static part changes), so the floor is committed again; room:gb,
    // unchanged, keeps its pictures (its new collection takes its arrays: nothing uploaded again), the walker stands
    // where it stood (in room:gb's frame, which did not move), the CRT bound again to room:gb's.
    const rv::world::StoredPlan before = building.plan();
    REQUIRE(building.fixture.applyOverlay("additions").has_value());
    const fs::path script = aggiornaScript(building.dir);
    const fs::path liveShot = building.dir / "live.png";
    const AppRun live =
        runApp(retroverseExe(), building.dir, "live", floorArguments(gb, 240, liveShot, {"--input-script", arg(script)}));
    requireCleanRun(live);
    requireFloorShown(live, building, "handheld");
    INFO(live.describe());
    const rv::app::RunReport& report = *live.report;
    REQUIRE(report.catalog.has_value());
    REQUIRE(report.catalog->refreshes.size() == 1);
    CHECK(report.catalog->refreshes.front().outcome == "done");
    CHECK(report.catalog->refreshes.front().added > 0);
    const rv::world::StoredPlan after = building.plan();
    const std::size_t newTitles = placementsIn(after.plan, shared) - placementsIn(before.plan, shared);
    CHECK(newTitles == 24);
    const rv::app::WorldRecord& world = report.world;
    REQUIRE(world.liveUpdates.size() == 1);
    const rv::app::WorldLiveUpdateRecord& update = world.liveUpdates.front();
    CHECK(update.kind == "rebuild");
    CHECK(update.planWritten);
    CHECK(update.moved == 0);
    CHECK(update.removed == 0);
    CHECK(update.added == newTitles);
    const rv::app::RoomUpdateRecord& gbUpdate = roomUpdate(update.rooms, gb);
    CHECK(gbUpdate.kind == "rebuilt");
    CHECK(gbUpdate.kept == placementsIn(before.plan, gb));
    CHECK(gbUpdate.added == 0);
    CHECK(gbUpdate.moved == 0);
    const rv::app::RoomUpdateRecord& sharedUpdate = roomUpdate(update.rooms, shared);
    CHECK(sharedUpdate.kind == "rebuilt");
    CHECK(sharedUpdate.kept == placementsIn(before.plan, shared));
    CHECK(sharedUpdate.added == newTitles);
    CHECK(sharedUpdate.moved == 0);
    REQUIRE(update.walker.has_value());
    CHECK(update.walker->area == gb);
    CHECK(glm::length(update.walker->shift) == 0.0f);
    requireNothingUploadedAgain(report);
    for (const rv::app::RoomResidencyRecord& room : report.residency.rooms) {
        if (room.room == gb) {
            CHECK(room.uploads == 0); // its pictures passed to its new collection, none made again
        }
    }
    REQUIRE(second.report->finalCamera.has_value());
    REQUIRE(report.finalCamera.has_value());
    CHECK(glm::length(report.finalCamera->feet - second.report->finalCamera->feet) < 1e-4f);
    CHECK(world.screenRoom == gb);
    REQUIRE(report.video.binds.size() == 1);
    CHECK(report.video.binds.front().screen.starts_with(gb + "/"));
    CHECK(report.video.maxDecoderThreads <= 1);

    // The other floors only saved (ADR-0013 §9): megadrive in gen4's new shared room, psx's new titles in its room,
    // their items files written for those floors' next start.
    const rv::world::Room& gen4Shared = roomIn(after.plan, "area:shared:gen4");
    CHECK(gen4Shared.floor == "gen4");
    REQUIRE(gen4Shared.sections.size() == 1);
    CHECK(gen4Shared.sections.front().str() == "megadrive");
    CHECK(placementsIn(after.plan, "room:psx") > placementsIn(before.plan, "room:psx"));
    const auto psxItems = rv::app::readRoomItems(rv::app::roomItemsFile(
        building.data / "cache", rv::catalog::LibraryId::parse("esde").value(),
        rv::catalog::SystemName::parse("psx").value()));
    REQUIRE(psxItems.has_value());
    CHECK(psxItems->items.size() == placementsIn(after.plan, "room:psx"));
}

TEST_CASE("world floor app: a system entering the resident floor rebuilds it with the walker kept in the hall's frame; "
          "another floor's new titles, saved meanwhile, shown at its next start",
          "[gpu][world][world_floor_app]") {
    const PlannedBuilding building("world_floor_app/live_gen4", false);
    const std::string snes = building.roomOf("snes");
    CHECK(snes == "room:snes");
    const AppRun first = runApp(retroverseExe(), building.dir, "first", floorArguments(snes, 2, std::nullopt));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const rv::world::StoredPlan before = building.plan();
    CHECK(rv::world::roomsOnFloor(before.plan, "gen4").size() == 1);

    // The walker in gen4's hall, in front of room:snes's doorway, facing it (in room:snes's frame, the scene's).
    const rv::world::FloorSceneBuild floorScene = floorSceneOf(before.plan, "gen4", first.report->assetRoot);
    const rv::world::RoomPlacement snesAt = roomIn(before.plan, snes).placement;
    const Doorway door = doorwayOf(floorScene.hall, snes);
    const glm::vec3 hallFeet = inRoomFrame(glm::vec3(door.x, 0.0f, hallCentreZ(floorScene.hall)), snesAt);
    const double yaw = wrapped((door.into < 0.0f ? 0.0 : 180.0) + (snesAt.yawHalfTurns % 2 == 0 ? 0.0 : 180.0));
    const rv::app::SpawnPoint spawn{hallFeet, static_cast<float>(yaw), 0.0f};

    // The additions imported by "Aggiorna libreria" while gen4 shows: megadrive (12 titles, under the threshold) enters
    // gen4's new shared room in the south row, snes's new titles take a dressed level of its room: the floor committed
    // again, the walker kept in the hall (its frame, the floor's, did not move under the scene's), nothing that stood
    // moved, room:snes's pictures kept.
    REQUIRE(building.fixture.applyOverlay("additions").has_value());
    const fs::path script = aggiornaScript(building.dir);
    const AppRun live =
        runApp(retroverseExe(), building.dir, "live",
               floorArguments(snes, 240, building.dir / "live.png",
                              {"--spawn", spawnArgument(spawn), "--input-script", arg(script)}));
    requireCleanRun(live);
    requireFloorShown(live, building, "gen4");
    INFO(live.describe());
    const rv::app::RunReport& report = *live.report;
    const rv::world::StoredPlan after = building.plan();
    const std::string entered = building.roomOf("megadrive");
    CHECK(entered == "area:shared:gen4");
    CHECK(rv::world::roomsOnFloor(after.plan, "gen4").size() == 2);
    const rv::app::WorldRecord& world = report.world;
    REQUIRE(world.rooms.size() == 2);
    REQUIRE(world.liveUpdates.size() == 1);
    const rv::app::WorldLiveUpdateRecord& update = world.liveUpdates.front();
    CHECK(update.kind == "rebuild");
    CHECK(update.moved == 0);
    CHECK(update.removed == 0);
    const std::size_t snesNew = placementsIn(after.plan, snes) - placementsIn(before.plan, snes);
    CHECK(snesNew > 0);
    CHECK(placementsIn(after.plan, entered) == 12);
    CHECK(update.added == snesNew + 12);
    const rv::app::RoomUpdateRecord& snesUpdate = roomUpdate(update.rooms, snes);
    CHECK(snesUpdate.kind == "rebuilt");
    CHECK(snesUpdate.kept == placementsIn(before.plan, snes));
    CHECK(snesUpdate.added == snesNew);
    CHECK(snesUpdate.moved == 0);
    CHECK(roomUpdate(update.rooms, entered).kind == "added");
    REQUIRE(update.walker.has_value());
    CHECK(update.walker->area == "hall:gen4");
    CHECK(glm::length(update.walker->shift) == 0.0f);
    REQUIRE(report.finalCamera.has_value());
    CHECK(glm::length(report.finalCamera->feet - spawn.feet) < 1e-4f);
    CHECK(world.room.empty()); // the eye in the hall
    requireNothingUploadedAgain(report);
    // The CRT: room:snes's from the start, kept in the hall, bound again to room:snes's in the floor committed again.
    CHECK(world.screenRoom == snes);
    REQUIRE(report.video.binds.size() == 1);
    CHECK(report.video.binds.front().screen.starts_with(snes + "/"));
    CHECK(report.video.maxDecoderThreads <= 1);

    // gen5 at its next start: psx's new titles, saved meanwhile with their items, shown at the first frame from the plan
    // and the items files, then reconciled with nothing to change but the covers' sources checked. gen5 never showed
    // before: its covers are not in the cache yet, so the preview shows their generated labels until the reconcile,
    // the media root known, makes the covers (uploaded over the labels).
    const std::string psx = building.roomOf("psx");
    const AppRun gen5 = runApp(retroverseExe(), building.dir, "gen5", floorArguments(psx, 40, std::nullopt));
    requireCleanRun(gen5);
    requireFloorShown(gen5, building, "gen5");
    INFO(gen5.describe());
    const rv::app::WorldRecord& psxWorld = gen5.report->world;
    CHECK(psxWorld.outcome == "unchanged");
    REQUIRE(psxWorld.preview.has_value());
    CHECK(psxWorld.preview->items == placementsIn(after.plan, psx));
    CHECK(psxWorld.preview->itemsKnown == placementsIn(after.plan, psx));
    REQUIRE(psxWorld.reconcile.has_value());
    CHECK(psxWorld.reconcile->kind == "textures");
    CHECK(psxWorld.reconcile->added == 0);
    CHECK(psxWorld.reconcile->moved == 0);
    CHECK(gen5.report->residency.covers == placementsIn(after.plan, psx));
}
