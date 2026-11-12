// The generated room's look (I3 step 16, style §4.1, §4.2, §8.1, §26.2): pixel probes on screenshots of the snes-room
// fixture's room, in both lighting profiles (world.json's "lightingProfile"; switching one moves nothing):
//   - from the doorway: every collection module's eye-height band (1.1 to 1.5 m) at least a third of the hero wall's
//     luminance (its modules over their height and the wall behind the hero corner); the upper walls (2.6 to 3.0 m)
//     darker than the modules' bands; the sign and the CRT the most saturated regions of the view, the collection's
//     items and the posters' prints aside (pictures; the 90th percentile of their chroma above every other tile's);
//   - close to a display case (0.9 m in front of its first face-out item): the covers behind the glass keep their darks
//     (no haze: the glass adds at most a few steps to the darkest pixels of the items behind it) and their contrast.
// I12 step 1 (ADR-0021 §1): the room's architecture and pendants from the doorway: the scene holds the papered walls,
// the picture rail, the cornice, the beams, a drum near the doorway and a spot can over each display case with the
// look pendant's light among the lights the view packs; the papered walls and the frieze above the rail never fall to
// black, the cans' lenses glow far above them.
// The probes read luminance as linear light (sRGB decoded) and chroma as max - min of the encoded bytes.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/run_report.hpp>
#include <rv/app/shelf_labels.hpp>
#include <rv/audio/analysis.hpp>
#include <rv/audio/wav_file.hpp>
#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/scene/atmosphere.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/gallery.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "image_checks.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
constexpr float kEyeHeight = 1.65f;
/// The chroma quantile the saturation probe compares (chromaQuantile).
constexpr double kChromaShare = 0.9;
/// How much more saturated than the CRT a poster's print may read (its 90th percentile; I6 step 1).
constexpr double kPosterChromaAllowance = 8.0;

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The snes-room fixture recorded as the library of <caseDir>/data and imported with retroverse-cli.
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedRoom(std::string_view name, std::string_view fixtureName = "esde/snes-room",
                          std::string_view overlay = {})
        : fixture(rv::test::Fixture::materialize(fixtureName).value()),
          dir(freshCaseDir(std::string("world_look/") + std::string(name))), data(dir / "data") {
        if (!overlay.empty()) {
            REQUIRE(fixture.applyOverlay(overlay).has_value());
        }
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    void chooseProfile(std::string_view profile) const {
        fs::create_directories(data / "config");
        std::ofstream config(data / "config" / "world.json", std::ios::binary);
        config << "{\"lightingProfile\": \"" << profile << "\"}\n";
    }
};

std::vector<std::string> roomArguments(std::string_view frames, const fs::path& shot,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", "room:snes", "--exit-after-frames", "--frames", std::string(frames), "--fixed-dt", "0.016667",
                                       "--screenshot",        arg(shot)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// The plan the run shows, its scene data with `profile` and its scene and collection.
struct ShownRoom {
    rv::world::StoredPlan stored;
    rv::world::SceneData data;
    rv::scene::SceneDescription scene;
    rv::world::CollectionLayout layout;
    rv::world::RoomId room;
};

ShownRoom shownRoom(const ImportedRoom& room, const rv::app::RunReport& report, std::string_view profile) {
    const fs::path planFile =
        rv::world::worldFiles(room.data, rv::catalog::LibraryId::parse(report.world.library).value()).plan;
    auto stored = rv::world::readStoredPlan(planFile);
    REQUIRE(stored.has_value());
    const rv::world::RoomId roomId = rv::world::RoomId::parse(report.world.room).value();
    auto lighting = rv::world::loadLightingProfile(report.assetRoot, profile);
    REQUIRE(lighting.has_value());
    auto data = rv::world::loadSceneData(report.assetRoot, stored->plan, roomId, *lighting);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, roomId, *data);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, roomId, *data, {});
    REQUIRE(layout.has_value());
    return ShownRoom{std::move(*stored), std::move(*data), std::move(*scene), std::move(*layout), roomId};
}

double linear(std::uint8_t value) {
    const double c = value / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

const std::uint8_t* pixelAt(const rv::assets::ImageData& image, int x, int y) {
    return image.pixels.data() +
           (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
               static_cast<std::size_t>(image.channels);
}

/// Linear luminance of the (2 r + 1)^2 window around `center`.
double windowLuminance(const rv::assets::ImageData& image, glm::ivec2 center, int radius) {
    double sum = 0.0;
    int count = 0;
    for (int y = std::max(0, center.y - radius); y <= std::min(image.height - 1, center.y + radius); ++y) {
        for (int x = std::max(0, center.x - radius); x <= std::min(image.width - 1, center.x + radius); ++x) {
            const std::uint8_t* p = pixelAt(image, x, y);
            sum += 0.2126 * linear(p[0]) + 0.7152 * linear(p[1]) + 0.0722 * linear(p[2]);
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

/// A rectangle of pixels, [min, max).
struct PixelRect {
    glm::ivec2 min{0};
    glm::ivec2 max{0};

    [[nodiscard]] bool empty() const noexcept { return max.x <= min.x || max.y <= min.y; }
    [[nodiscard]] bool overlaps(const PixelRect& other) const noexcept {
        return min.x < other.max.x && other.min.x < max.x && min.y < other.max.y && other.min.y < max.y;
    }
};

/// The `share` quantile of the chroma (max - min of the encoded bytes) of the pixels in `rect`: how saturated its most
/// saturated tenth (0.9) reads, whatever its dark background (the sign's lettering on black).
double chromaQuantile(const rv::assets::ImageData& image, const PixelRect& rect, double share) {
    std::vector<int> values;
    for (int y = rect.min.y; y < rect.max.y; ++y) {
        for (int x = rect.min.x; x < rect.max.x; ++x) {
            const std::uint8_t* p = pixelAt(image, x, y);
            values.push_back(std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}));
        }
    }
    if (values.empty()) {
        return 0.0;
    }
    const auto at = static_cast<std::size_t>(share * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
    return values[at];
}

/// The `share` quantile of the luma of the pixels in `rect`.
double lumaQuantile(const rv::assets::ImageData& image, const PixelRect& rect, double share) {
    std::vector<double> values;
    for (int y = rect.min.y; y < rect.max.y; ++y) {
        for (int x = rect.min.x; x < rect.max.x; ++x) {
            values.push_back(lumaAt(image, x, y));
        }
    }
    if (values.empty()) {
        return 0.0;
    }
    const auto at = static_cast<std::size_t>(share * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(at), values.end());
    return values[at];
}

const rv::world::Fixture* fixtureOf(const rv::world::WorldPlan& plan, std::string_view part,
                                    rv::world::FixtureRole role) {
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        if (fixture.role == role && fixture.id.part() == part) {
            return &fixture;
        }
    }
    return nullptr;
}

/// The fixture's prefab bounds (its own space).
rv::core::Aabb boundsOf(const ShownRoom& shown, const rv::world::Fixture& fixture) {
    const rv::world::ScenePrefab* prefab = shown.data.prefabs.find(fixture.prefab);
    REQUIRE(prefab != nullptr);
    REQUIRE(prefab->prefab.has_value());
    return prefab->prefab->bounds;
}

/// The points of the fixture's front face (its bounds' +Z face) at `columns` x `rows` positions of the rectangle
/// [u0, u1] x [y0, y1] (u across its width, 0 to 1; y in meters above the floor, clamped to the face).
std::vector<glm::vec3> frontPoints(const ShownRoom& shown, const rv::world::Fixture& fixture, float u0, float u1,
                                   float y0, float y1, int columns, int rows, float out = 0.0f) {
    const rv::core::Aabb bounds = boundsOf(shown, fixture);
    const glm::mat4 frame = rv::world::room_frames::fixtureTransform(fixture);
    std::vector<glm::vec3> points;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            const float u = u0 + (u1 - u0) * (static_cast<float>(c) + 0.5f) / static_cast<float>(columns);
            const float y = std::clamp(y0 + (y1 - y0) * (static_cast<float>(r) + 0.5f) / static_cast<float>(rows),
                                       bounds.min.y, bounds.max.y);
            const glm::vec3 local(bounds.min.x + u * (bounds.max.x - bounds.min.x), y - frame[3].y,
                                  bounds.max.z + out);
            points.emplace_back(frame * glm::vec4(local, 1.0f));
        }
    }
    return points;
}

/// Mean linear luminance of the visible ones of `points` (5 x 5 windows); nothing when none is in view.
std::optional<double> meanLuminance(const rv::assets::ImageData& image, const rv::scene::CameraSetup& camera,
                                    const std::vector<glm::vec3>& points) {
    double sum = 0.0;
    int count = 0;
    for (const glm::vec3& point : points) {
        if (const auto pixel = projectToPixel(camera, kShotSize, point)) {
            sum += windowLuminance(image, *pixel, 2);
            ++count;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return sum / count;
}

/// The pixel rectangle around the projections of `points` (empty when one is out of view).
PixelRect rectOf(const rv::scene::CameraSetup& camera, const std::vector<glm::vec3>& points) {
    PixelRect rect{glm::ivec2(kShotSize), glm::ivec2(0)};
    for (const glm::vec3& point : points) {
        const auto pixel = projectToPixel(camera, kShotSize, point);
        if (!pixel) {
            return PixelRect{};
        }
        rect.min = glm::min(rect.min, *pixel);
        rect.max = glm::max(rect.max, *pixel + 1);
    }
    return rect;
}

/// The walker 0.9 m in front of an item's face, looking at it, and the camera it gives.
struct Viewpoint {
    rv::app::SpawnPoint spawn;
    rv::scene::CameraSetup camera;
};

Viewpoint viewpointOf(const rv::world::CollectionItem& item, float distance, const rv::scene::CameraSetup& lens) {
    const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
    const glm::vec3 center(item.slotFrame * glm::vec4(local, 1.0f));
    glm::vec3 out(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    out.y = 0.0f;
    out = glm::normalize(out);
    const glm::vec3 eye(center.x + distance * out.x, kEyeHeight, center.z + distance * out.z);
    const glm::vec3 look = center - eye;
    Viewpoint viewpoint;
    viewpoint.spawn.feet = glm::vec3(eye.x, 0.0f, eye.z);
    viewpoint.spawn.yawDeg = rv::core::radiansToDegrees(std::atan2(look.x, -look.z));
    viewpoint.spawn.pitchDeg =
        rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
    viewpoint.camera = lens;
    viewpoint.camera.position = eye;
    viewpoint.camera.yawDeg = *viewpoint.spawn.yawDeg;
    viewpoint.camera.pitchDeg = *viewpoint.spawn.pitchDeg;
    return viewpoint;
}

/// The doorway's probes of one profile.
/// The view's rectangles of I12 step 4's shelves of colour as the scene draws them (a bookcase, a record rack, a
/// pegboard: their spines, sleeves and pads), which stand in front of the wall's pictures beside them too.
std::vector<PixelRect> tallPieceRects(const ShownRoom& shown, const rv::scene::CameraSetup& camera) {
    std::vector<PixelRect> rects;
    for (const rv::scene::SceneObject& object : shown.scene.objects) {
        if (object.kind != "bookcase" && object.kind != "record_rack" && object.kind != "pegboard") {
            continue;
        }
        auto built = rv::geometry::buildPrefab(object.kind, object.params);
        REQUIRE(built.has_value());
        std::vector<glm::vec3> corners;
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 local((i & 1) ? built->bounds.max.x : built->bounds.min.x,
                                  (i & 2) ? built->bounds.max.y : built->bounds.min.y,
                                  (i & 4) ? built->bounds.max.z : built->bounds.min.z);
            corners.emplace_back(object.transform * glm::vec4(local, 1.0f));
        }
        if (const PixelRect rect = rectOf(camera, corners); !rect.empty()) {
            rects.push_back(rect);
        }
    }
    return rects;
}

void checkDoorway(const ShownRoom& shown, const rv::assets::ImageData& image, std::string_view profile) {
    INFO("profile " << profile);
    const rv::scene::CameraSetup& camera = shown.scene.camera;
    const rv::world::WorldPlan& plan = shown.stored.plan;

    // The hero wall's luminance: its modules over their height and the wall behind the hero corner (around the CRT).
    std::vector<glm::vec3> heroWall;
    std::vector<const rv::world::Fixture*> modules;
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        if (fixture.role == rv::world::FixtureRole::HeroModule || fixture.role == rv::world::FixtureRole::SideModule) {
            modules.push_back(&fixture);
        }
        if (fixture.role == rv::world::FixtureRole::HeroModule) {
            const auto points = frontPoints(shown, fixture, 0.1f, 0.9f, 0.4f, 1.9f, 6, 8);
            heroWall.insert(heroWall.end(), points.begin(), points.end());
        }
    }
    REQUIRE_FALSE(modules.empty());
    const rv::world::Fixture* cabinet = fixtureOf(plan, "tv-cabinet", rv::world::FixtureRole::HeroCorner);
    REQUIRE(cabinet != nullptr);
    {
        // The wall behind the cabinet, above it (the cabinet's back is on the wall).
        const rv::core::Aabb bounds = boundsOf(shown, *cabinet);
        const glm::mat4 frame = rv::world::room_frames::fixtureTransform(*cabinet);
        for (int c = 0; c < 6; ++c) {
            for (int r = 0; r < 4; ++r) {
                const float u = (static_cast<float>(c) + 0.5f) / 6.0f;
                const glm::vec3 local(bounds.min.x + u * (bounds.max.x - bounds.min.x), 1.3f + 0.2f * static_cast<float>(r),
                                      bounds.min.z - 0.05f);
                heroWall.emplace_back(frame * glm::vec4(local, 1.0f));
            }
        }
    }
    const std::optional<double> heroLuminance = meanLuminance(image, camera, heroWall);
    REQUIRE(heroLuminance.has_value());

    double bandSum = 0.0;
    int bands = 0;
    for (const rv::world::Fixture* module : modules) {
        const auto band = meanLuminance(image, camera, frontPoints(shown, *module, 0.1f, 0.9f, 1.1f, 1.5f, 8, 3));
        if (!band) {
            continue; // behind the camera (none from the doorway of the base room)
        }
        INFO("module " << module->id.toString() << ": eye band " << *band << ", hero wall " << *heroLuminance);
        CHECK(*band >= *heroLuminance / 3.0);
        bandSum += *band;
        ++bands;
    }
    REQUIRE(bands > 0);
    const double bandMean = bandSum / bands;

    // The upper walls: the side walls and the hero wall between 2.6 and 3.0 m, clear of the sign.
    std::vector<glm::vec3> upper;
    const rv::world::Room& room = plan.rooms.front();
    const float minX = static_cast<float>(room.minXMm) / 1000.0f + 0.005f;
    const float maxX = static_cast<float>(room.maxXMm) / 1000.0f - 0.005f;
    const float minZ = static_cast<float>(room.minZMm) / 1000.0f + 0.005f;
    for (int i = 0; i < 8; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / 8.0f;
        for (const float y : {2.65f, 2.95f}) {
            upper.emplace_back(minX, y, minZ + t * 7.0f);
            upper.emplace_back(maxX, y, minZ + t * 7.0f);
            if (std::abs(minX + t * (maxX - minX)) > 1.5f) {
                upper.emplace_back(minX + t * (maxX - minX), y, minZ);
            }
        }
    }
    const std::optional<double> upperLuminance = meanLuminance(image, camera, upper);
    REQUIRE(upperLuminance.has_value());
    INFO("upper walls " << *upperLuminance << ", modules' eye bands " << bandMean);
    CHECK(*upperLuminance < bandMean);

    // The sign and the CRT: the most saturated regions of the view.
    const rv::world::Fixture* sign = fixtureOf(plan, "sign", rv::world::FixtureRole::HeroCorner);
    const rv::world::Fixture* crt = fixtureOf(plan, "crt", rv::world::FixtureRole::HeroCorner);
    REQUIRE(sign != nullptr);
    REQUIRE(crt != nullptr);
    // The sign's picture and the CRT's screen (its ScreenSurface socket), each a little inside its edges.
    const auto faceRect = [&](const glm::mat4& frame, const rv::core::Aabb& box, float z) {
        const glm::vec3 inset = 0.1f * box.size();
        return rectOf(camera, {glm::vec3(frame * glm::vec4(box.min.x + inset.x, box.min.y + inset.y, z, 1.0f)),
                               glm::vec3(frame * glm::vec4(box.max.x - inset.x, box.max.y - inset.y, z, 1.0f))});
    };
    const rv::core::Aabb signBounds = boundsOf(shown, *sign);
    const PixelRect signRect =
        faceRect(rv::world::room_frames::fixtureTransform(*sign), signBounds, signBounds.max.z);
    const rv::world::ScenePrefab* crtPrefab = shown.data.prefabs.find(crt->prefab);
    REQUIRE(crtPrefab != nullptr);
    REQUIRE(crtPrefab->prefab.has_value());
    const rv::geometry::Socket* screen = nullptr;
    for (const rv::geometry::Socket& candidate : crtPrefab->prefab->sockets) {
        if (candidate.id == "ScreenSurface") {
            screen = &candidate;
        }
    }
    REQUIRE(screen != nullptr);
    const PixelRect crtRect =
        faceRect(rv::world::room_frames::fixtureTransform(*crt) * screen->transform, screen->bounds, 0.0f);
    // The whole CRT and every item of the collection are left out of the other tiles: the covers are the user's own
    // pictures (as saturated as they come), the sign and the screen are what the room itself makes stand out.
    const rv::core::Aabb crtBounds = crtPrefab->prefab->bounds;
    const glm::mat4 crtFrame = rv::world::room_frames::fixtureTransform(*crt);
    std::vector<PixelRect> excluded{rectOf(camera, {glm::vec3(crtFrame * glm::vec4(crtBounds.min.x, crtBounds.min.y, crtBounds.max.z, 1.0f)),
                                                    glm::vec3(crtFrame * glm::vec4(crtBounds.max.x, crtBounds.max.y, crtBounds.max.z, 1.0f))}),
                                    signRect};
    for (const rv::world::CollectionItem& item : shown.layout.items) {
        std::vector<glm::vec3> corners;
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 local((i & 1) ? item.slotBox.max.x : item.slotBox.min.x,
                                  (i & 2) ? item.slotBox.max.y : item.slotBox.min.y,
                                  (i & 4) ? item.slotBox.max.z : item.slotBox.min.z);
            corners.emplace_back(item.slotFrame * glm::vec4(local, 1.0f));
        }
        if (const PixelRect rect = rectOf(camera, corners); !rect.empty()) {
            excluded.push_back(PixelRect{rect.min - 2, rect.max + 2});
        }
    }
    // The posters' prints too (I6 step 1): invented covers, pictures as saturated as the collection's, which the
    // brighter walls since the doorway light (ADR-0014 §4) show nearer their own colour. What the check guards is the
    // room's light: the sign and the screen stand out from every lit surface that is not a picture; the posters are
    // held apart, never more saturated than the CRT by more than kPosterChromaAllowance.
    std::vector<PixelRect> posters;
    for (const rv::world::Fixture& fixture : plan.fixtures) {
        if (fixture.role != rv::world::FixtureRole::Decoration || fixture.prefab.kind != "poster_frame") {
            continue;
        }
        const rv::core::Aabb bounds = boundsOf(shown, fixture);
        const glm::mat4 frame = rv::world::room_frames::fixtureTransform(fixture);
        std::vector<glm::vec3> corners;
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 local((i & 1) ? bounds.max.x : bounds.min.x, (i & 2) ? bounds.max.y : bounds.min.y,
                                  (i & 4) ? bounds.max.z : bounds.min.z);
            corners.emplace_back(frame * glm::vec4(local, 1.0f));
        }
        if (const PixelRect rect = rectOf(camera, corners); !rect.empty()) {
            excluded.push_back(PixelRect{rect.min - 2, rect.max + 2});
            posters.push_back(PixelRect{glm::max(rect.min, glm::ivec2(0)), glm::min(rect.max, kShotSize)});
        }
    }
    // I12 step 4's shelves of colour, alike: a bookcase's spines, a rack's sleeves, a pegboard's pads (the scene's
    // pieces as drawn, not the plan's prefabs).
    for (const PixelRect& rect : tallPieceRects(shown, camera)) {
        excluded.push_back(PixelRect{rect.min - 2, rect.max + 2});
        posters.push_back(PixelRect{glm::max(rect.min, glm::ivec2(0)), glm::min(rect.max, kShotSize)});
    }
    REQUIRE_FALSE(signRect.empty());
    REQUIRE_FALSE(crtRect.empty());
    const double signChroma = chromaQuantile(image, signRect, kChromaShare);
    const double crtChroma = chromaQuantile(image, crtRect, kChromaShare);
    // Tiles of the sign's height elsewhere in the view.
    const int tile = std::max(24, signRect.max.y - signRect.min.y);
    double otherChroma = 0.0;
    glm::ivec2 otherAt{0};
    for (int y = 0; y + tile <= kShotSize.y; y += tile / 2) {
        for (int x = 0; x + tile <= kShotSize.x; x += tile / 2) {
            const PixelRect rect{{x, y}, {x + tile, y + tile}};
            if (std::any_of(excluded.begin(), excluded.end(),
                            [&](const PixelRect& other) { return rect.overlaps(other); })) {
                continue;
            }
            const double chroma = chromaQuantile(image, rect, kChromaShare);
            if (chroma > otherChroma) {
                otherChroma = chroma;
                otherAt = rect.min;
            }
        }
    }
    INFO("chroma: sign " << signChroma << ", CRT " << crtChroma << ", the most saturated other tile " << otherChroma
                         << " at (" << otherAt.x << ", " << otherAt.y << "), tiles of " << tile << " px");
    CHECK(signChroma > otherChroma);
    CHECK(crtChroma > otherChroma);
    double posterChroma = 0.0;
    for (const PixelRect& rect : posters) {
        if (!rect.empty()) {
            posterChroma = std::max(posterChroma, chromaQuantile(image, rect, kChromaShare));
        }
    }
    INFO("the most saturated poster " << posterChroma);
    CHECK(posterChroma <= crtChroma + kPosterChromaAllowance);
    WARN("look probes (" << profile << "): hero wall " << *heroLuminance << ", eye bands " << bandMean << ", upper walls "
                         << *upperLuminance << "; chroma sign " << signChroma << ", CRT " << crtChroma
                         << ", other tiles at most " << otherChroma << ", posters at most " << posterChroma);
}

/// Close to a display case: the covers behind its glass keep their darks and their contrast.
void checkDisplayGlass(const ShownRoom& shown, const rv::assets::ImageData& image, const Viewpoint& view,
                       std::string_view profile) {
    INFO("profile " << profile);
    std::size_t probed = 0;
    std::vector<double> darks;
    for (const rv::world::CollectionItem& item : shown.layout.items) {
        if (item.slot.fixture().socket() != "dc01") {
            continue;
        }
        const glm::vec3 lo(item.slotFrame * glm::vec4(item.slotBox.min.x * 0.8f, item.slotBox.min.y + 0.1f * item.slotBox.size().y, item.slotBox.max.z, 1.0f));
        const glm::vec3 hi(item.slotFrame * glm::vec4(item.slotBox.max.x * 0.8f, item.slotBox.max.y - 0.1f * item.slotBox.size().y, item.slotBox.max.z, 1.0f));
        const PixelRect rect = rectOf(view.camera, {lo, hi});
        if (rect.empty() || (rect.max.x - rect.min.x) < 20) {
            continue;
        }
        ++probed;
        const double dark = lumaQuantile(image, rect, 0.05);
        const double bright = lumaQuantile(image, rect, 0.95);
        INFO("item " << item.game.toString() << ": luma p5 " << dark << ", p95 " << bright);
        // Under the case's accent spot (step 17a's review) a pastel cover without darks of its own reads its darkest
        // 5% above 70: each item is held to a looser ceiling, the median item to the haze bound (a hazy pane would
        // lift every cover's darks).
        CHECK(dark <= 120.0);
        CHECK(bright - dark >= 60.0);
        darks.push_back(dark);
    }
    CHECK(probed >= 3);
    if (!darks.empty()) {
        std::sort(darks.begin(), darks.end());
        const double median = darks[darks.size() / 2];
        INFO("the median item's luma p5 " << median);
        CHECK(median <= 70.0);
    }
}

} // namespace

TEST_CASE("world look: the snes-room's light levels, saturation and clear display glass in both lighting profiles",
          "[gpu][world][world_look]") {
    ImportedRoom room("profiles");
    std::optional<std::string> firstDigest;
    for (const std::string_view profile : {std::string_view("evening"), std::string_view("afternoon")}) {
        room.chooseProfile(profile);
        const fs::path doorway = room.dir / (std::string(profile) + "_doorway.png");
        const AppRun run = runApp(retroverseExe(), room.dir, std::string(profile) + "_doorway", roomArguments("40", doorway));
        skipWithoutOpenGl(run);
        requireCleanRun(run);
        INFO(run.describe());
        REQUIRE(run.report->world.bound);
        CHECK(run.report->world.state == "room");
        // The room's lights (the eye's area's on its floor, I4 step 5a: the hall's are the floor's too) within the
        // renderer's 16.
        REQUIRE(run.report->sceneAreas.has_value());
        CHECK(run.report->sceneAreas->area == "room:snes");
        CHECK(run.report->sceneAreas->lights.size() <= 16);
        // A profile is how the room is drawn: the second run shows the first one's plan unchanged.
        if (firstDigest) {
            CHECK(run.report->world.outcome == "unchanged");
            CHECK(run.report->world.planDigest == *firstDigest);
        } else {
            firstDigest = run.report->world.planDigest;
        }
        const ShownRoom shown = shownRoom(room, *run.report, profile);
        const rv::assets::ImageData image = requireScreenshot(run, doorway, kShotSize);
        checkDoorway(shown, image, profile);

        // Close to the first display case's first face-out item.
        const auto item = std::find_if(shown.layout.items.begin(), shown.layout.items.end(),
                                       [](const rv::world::CollectionItem& candidate) {
                                           return candidate.slot.fixture().socket() == "dc01";
                                       });
        REQUIRE(item != shown.layout.items.end());
        const Viewpoint view = viewpointOf(*item, 0.9f, shown.scene.camera);
        const fs::path close = room.dir / (std::string(profile) + "_display.png");
        const AppRun closeRun = runApp(retroverseExe(), room.dir, std::string(profile) + "_display",
                                       roomArguments("12", close, {"--spawn", spawnArgument(view.spawn)}));
        requireCleanRun(closeRun);
        checkDisplayGlass(shown, requireScreenshot(closeRun, close, kShotSize), view, profile);
    }
}

TEST_CASE("world look: the shell and the lamps (I12 step 1): papered walls, a rail, a cornice, beams, lit spot cans",
          "[gpu][world][world_look]") {
    ImportedRoom room("shell");
    const fs::path doorway = room.dir / "doorway.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "doorway", roomArguments("40", doorway));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.bound);
    const ShownRoom shown = shownRoom(room, *run.report, "evening");
    const rv::assets::ImageData image = requireScreenshot(run, doorway, kShotSize);
    const rv::scene::CameraSetup& camera = shown.scene.camera;
    const rv::world::Room& planned = shown.stored.plan.rooms.front();

    // The classic pack's architecture on every wall (the doorway's wall papered beside and over its casing), a beam
    // every 3 m from the north wall short of the south one, a drum of five parts near the doorway, a can of three over
    // each display case.
    const auto count = [&](std::string_view prefix) {
        return std::count_if(shown.scene.objects.begin(), shown.scene.objects.end(),
                             [&](const rv::scene::SceneObject& object) { return object.id.starts_with(prefix); });
    };
    CHECK(count("dressing:shell/wallpaper-") == 6);
    CHECK(count("dressing:shell/rail-") == 4);
    CHECK(count("dressing:shell/cornice-") == 4);
    CHECK(count("dressing:shell/beam:") == (planned.maxZMm - planned.minZMm - 1000) / 3000);
    CHECK(count("dressing:pendant:doorway/") == 5);
    CHECK(count("dressing:pendant:fx:dc01/") == 3);
    CHECK(count("dressing:pendant:fx:dc02/") == 3);
    // The look pendant's light: in the scene, and packed for the view at the doorway (the eye's area's nearest).
    CHECK(std::any_of(shown.scene.lights.begin(), shown.scene.lights.end(),
                      [](const rv::scene::Light& light) { return light.name == "look:pendant"; }));
    REQUIRE(run.report->sceneAreas.has_value());
    const std::vector<std::string>& packed = run.report->sceneAreas->lights;
    CHECK(std::find(packed.begin(), packed.end(), "room:snes/look:pendant") != packed.end());
    CHECK(packed.size() <= 16);

    // The papered walls between the shelves and the rail (1.9 and 2.4 m) and the frieze above it (2.9 m) on the side
    // walls and on the hero wall clear of the sign: their mean luminance never black.
    std::vector<glm::vec3> paper;
    std::vector<glm::vec3> frieze;
    const float minX = static_cast<float>(planned.minXMm) / 1000.0f + 0.01f;
    const float maxX = static_cast<float>(planned.maxXMm) / 1000.0f - 0.01f;
    const float minZ = static_cast<float>(planned.minZMm) / 1000.0f + 0.01f;
    for (int i = 0; i < 8; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / 8.0f;
        for (const float y : {1.9f, 2.4f, 2.9f}) {
            std::vector<glm::vec3>& band = y > 2.7f ? frieze : paper;
            band.emplace_back(minX, y, minZ + t * 7.0f);
            band.emplace_back(maxX, y, minZ + t * 7.0f);
            if (std::abs(minX + t * (maxX - minX)) > 1.6f) {
                band.emplace_back(minX + t * (maxX - minX), y, minZ);
            }
        }
    }
    const std::optional<double> paperLuminance = meanLuminance(image, camera, paper);
    const std::optional<double> friezeLuminance = meanLuminance(image, camera, frieze);
    REQUIRE(paperLuminance.has_value());
    REQUIRE(friezeLuminance.has_value());
    INFO("papered walls " << *paperLuminance << ", frieze " << *friezeLuminance);
    // Measured at the step (linear): the paper 0.022, the frieze 0.0074 (luma about 40 and 21); the floors are about
    // half of them, the frieze's above style §4's black (luma 12).
    CHECK(*paperLuminance > 0.012);
    CHECK(*friezeLuminance > 0.004);
    CHECK(*paperLuminance > *friezeLuminance);

    // The spot cans over the display cases glow: their lenses, seen from below.
    for (const std::string_view socket : {std::string_view("dc01"), std::string_view("dc02")}) {
        const auto found =
            std::find_if(shown.stored.plan.fixtures.begin(), shown.stored.plan.fixtures.end(),
                         [&](const rv::world::Fixture& fixture) { return fixture.id.socket() == socket; });
        REQUIRE(found != shown.stored.plan.fixtures.end());
        const rv::world::Fixture* cas = &*found;
        const glm::vec3 lens(static_cast<float>(cas->xMm) / 1000.0f, 2.351f, static_cast<float>(cas->zMm) / 1000.0f);
        const std::optional<double> glow = meanLuminance(image, camera, {lens});
        REQUIRE(glow.has_value());
        INFO("lens over " << socket << ": " << *glow);
        // Lit, far above the walls around it.
        CHECK(*glow > 0.3);
        CHECK(*glow > 10.0 * *paperLuminance);
    }
}

TEST_CASE("world look: the gallery (I12 step 2): the room's own media in its frames, the prints when it is off",
          "[gpu][world][world_look]") {
    // The fixture's gallery overlay (I12 step 5): since step 4 frames hang over the low decoration too, more than the
    // fixture's one game with fanart, marquee and screenshot could fill.
    ImportedRoom room("gallery", "esde/snes-room", "gallery");
    const fs::path withMedia = room.dir / "gallery_on.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "gallery_on", roomArguments("40", withMedia));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.bound);
    REQUIRE(run.report->catalog.has_value());
    const ShownRoom shown = shownRoom(room, *run.report, "evening");
    const rv::scene::CameraSetup& camera = shown.scene.camera;

    // The room's frames (one over each module), their pictures' centres on their front.
    std::vector<glm::vec3> centres;
    for (const rv::scene::SceneObject& object : shown.scene.objects) {
        if (object.kind == "poster_frame" && object.id.starts_with(rv::world::kGalleryObjectPrefix)) {
            const glm::vec3 front = glm::normalize(glm::vec3(object.transform[2]));
            centres.push_back(glm::vec3(object.transform[3]) + 0.031f * front);
        }
    }
    REQUIRE_FALSE(centres.empty());

    // Every frame a medium of the room's games (the fixture's covers serve the portraits, its overlay's fanart,
    // marquees and screenshots the others), shown, none failing; the pictures' bytes the gallery's kind of GPU memory.
    const rv::app::CatalogRecord& on = *run.report->catalog;
    CHECK(on.gallery);
    CHECK(on.galleryFrames == centres.size());
    CHECK(on.galleryWithMedia == on.galleryFrames);
    CHECK(on.galleryShown == on.galleryWithMedia);
    CHECK(on.galleryFailed == 0);
    CHECK(on.galleryBytes > 0);
    CHECK(on.galleryBytes == run.report->gpuMemory.usage(rv::gl::GpuMemoryKind::GalleryTextures).bytes);
    // At most about 0.4 MiB a picture (BC1 with its mips; RGBA8 without S3TC is four times that).
    CHECK(on.galleryBytes <= on.galleryShown * 4u * 1024u * 1024u);

    // "Galleria della stanza" off (world.json): the same frames, every one its print, nothing on the GPU for them.
    fs::create_directories(room.data / "config");
    {
        std::ofstream config(room.data / "config" / "world.json", std::ios::binary);
        config << "{\"gallery\": false}\n";
    }
    const fs::path prints = room.dir / "gallery_off.png";
    const AppRun offRun = runApp(retroverseExe(), room.dir, "gallery_off", roomArguments("40", prints));
    requireCleanRun(offRun);
    REQUIRE(offRun.report->catalog.has_value());
    const rv::app::CatalogRecord& off = *offRun.report->catalog;
    CHECK_FALSE(off.gallery);
    CHECK(off.galleryFrames == on.galleryFrames);
    CHECK(off.galleryWithMedia == 0);
    CHECK(off.galleryShown == 0);
    CHECK(off.galleryBytes == 0);
    CHECK(offRun.report->gpuMemory.usage(rv::gl::GpuMemoryKind::GalleryTextures).peakBytes == 0);

    // The frames' pictures are not the prints: each frame's centre differs between the two views.
    const rv::assets::ImageData mediaImage = requireScreenshot(run, withMedia, kShotSize);
    const rv::assets::ImageData printImage = requireScreenshot(offRun, prints, kShotSize);
    std::size_t seen = 0;
    const std::vector<PixelRect> tall = tallPieceRects(shown, camera); // a frame behind a bookcase from the doorway
    for (const glm::vec3& centre : centres) {
        const auto pixel = projectToPixel(camera, kShotSize, centre);
        if (!pixel || std::any_of(tall.begin(), tall.end(), [&](const PixelRect& rect) {
                return rect.overlaps(PixelRect{*pixel, *pixel + 1});
            })) {
            continue;
        }
        ++seen;
        INFO("frame at (" << pixel->x << ", " << pixel->y << ")");
        CHECK(std::abs(windowLuminance(mediaImage, *pixel, 2) - windowLuminance(printImage, *pixel, 2)) > 0.002);
    }
    CHECK(seen > 0);
}

namespace {

/// The luminance range (linear) of the (2 r + 1)^2 window around `center`: ink on paper reads as a wide one.
double luminanceRange(const rv::assets::ImageData& image, glm::ivec2 center, int radius) {
    double low = 1.0;
    double high = 0.0;
    for (int y = std::max(0, center.y - radius); y <= std::min(image.height - 1, center.y + radius); ++y) {
        for (int x = std::max(0, center.x - radius); x <= std::min(image.width - 1, center.x + radius); ++x) {
            const std::uint8_t* p = pixelAt(image, x, y);
            const double value = 0.2126 * linear(p[0]) + 0.7152 * linear(p[1]) + 0.0722 * linear(p[2]);
            low = std::min(low, value);
            high = std::max(high, value);
        }
    }
    return high - low;
}

/// --spawn's text for a walker whose eye looks at `target` from `distance` metres before it along `facing`'s
/// horizontal part (yaw 0 looks along -Z, -90 along -X; pitch down is negative).
std::string spawnLookingAt(const glm::vec3& target, const glm::vec3& facing, float distance) {
    const glm::vec3 out = glm::normalize(glm::vec3(facing.x, 0.0f, facing.z));
    const glm::vec3 feet = target + out * distance;
    const glm::vec3 eye(feet.x, 1.65f, feet.z);
    const glm::vec3 look = target - eye;
    const double yaw = glm::degrees(std::atan2(static_cast<double>(look.x), static_cast<double>(-look.z)));
    const double pitch = glm::degrees(
        std::atan2(static_cast<double>(look.y), std::sqrt(static_cast<double>(look.x * look.x + look.z * look.z))));
    return std::to_string(feet.x) + ",0," + std::to_string(feet.z) + "," + std::to_string(yaw) + "," +
           std::to_string(pitch);
}

} // namespace

TEST_CASE("world look: the shelves' detail (I12 step 5): letter tabs and caption cards, legible up close",
          "[gpu][world][world_look][shelf_labels]") {
    ImportedRoom room("labels", "esde/snes-stress");
    const fs::path doorway = room.dir / "labels_door.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "labels_door", roomArguments("40", doorway));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.bound);
    REQUIRE(run.report->catalog.has_value());
    const ShownRoom shown = shownRoom(room, *run.report, "evening");

    // The labels the room should have: every placed game's text is known (the fixture's catalog), so a tab per
    // spine-out level of a module with an item, a card per face-out item of a display case.
    rv::app::RoomShelfTexts texts;
    for (const rv::world::CollectionItem& item : shown.layout.items) {
        rv::app::ShelfText text;
        text.game = item.game;
        text.title = "T";
        text.initial = "A";
        texts.games.push_back(text);
    }
    std::sort(texts.games.begin(), texts.games.end(),
              [](const rv::app::ShelfText& a, const rv::app::ShelfText& b) { return a.game < b.game; });
    texts.games.erase(
        std::unique(texts.games.begin(), texts.games.end(),
                    [](const rv::app::ShelfText& a, const rv::app::ShelfText& b) { return a.game == b.game; }),
        texts.games.end());
    const rv::app::ShelfLabels labels =
        rv::app::roomShelfLabels(shown.stored.plan, shown.room, shown.data, shown.layout, {}, texts);
    REQUIRE(labels.tabs.size() > 20);
    REQUIRE_FALSE(labels.captions.empty());
    const rv::app::CatalogRecord& record = *run.report->catalog;
    CHECK(record.shelfTabs == labels.tabs.size());
    CHECK(record.shelfCaptions == labels.captions.size());
    CHECK(record.sectionSigns == 0);
    CHECK(record.shelfAtlases == 2);
    CHECK(record.shelfAtlasesShown == record.shelfAtlases);
    CHECK(record.shelfLabelsFailed == 0);
    CHECK(record.shelfLabelBytes > 0);
    CHECK(record.shelfLabelBytes == run.report->gpuMemory.usage(rv::gl::GpuMemoryKind::LabelTextures).bytes);
    // The room's two atlases within 2 MiB (BC1 with their mips; RGBA8 without S3TC is eight times that).
    CHECK(record.shelfLabelBytes <= 16u * 1024u * 1024u);

    // Up close (0.7 m), the first module's first tab and the first card read: ink on their paper at the view's centre.
    struct Close {
        const char* name;
        glm::mat4 frame;
        float face;
    };
    for (const Close& close : {Close{"tab", labels.tabs.front().frame, rv::app::kPlateThickness},
                               Close{"card", labels.captions.front().frame, rv::app::kPlateThickness}}) {
        const glm::vec3 facing(close.frame[2]);
        const glm::vec3 target = glm::vec3(close.frame[3]) + facing * close.face;
        const std::string spawn = spawnLookingAt(target, facing, 0.7f);
        const fs::path shot = room.dir / (std::string("labels_") + close.name + ".png");
        const AppRun closeRun = runApp(retroverseExe(), room.dir, std::string("labels_") + close.name,
                                       roomArguments("40", shot, {"--spawn", spawn}));
        requireCleanRun(closeRun);
        const rv::assets::ImageData image = requireScreenshot(closeRun, shot, kShotSize);
        const double range = luminanceRange(image, kShotSize / 2, 14);
        INFO(close.name << " from " << spawn << ": the luminance range " << range);
        CHECK(range > 0.08);
    }
}

TEST_CASE("world look: a shared room's section signs (I12 step 5)", "[gpu][world][world_look][shelf_labels]") {
    ImportedRoom room("sections", "esde/building");
    const fs::path doorway = room.dir / "sections_door.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "sections_door",
                              {"--start-room", "area:shared:handheld", "--exit-after-frames", "--frames", "40",
                               "--fixed-dt", "0.016667", "--screenshot", arg(doorway)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->catalog.has_value());
    // Its two sections (pocketlab's and pokemini's), a sign each; the floor's console rooms have none.
    CHECK(run.report->catalog->sectionSigns == 2);
    CHECK(run.report->catalog->shelfLabelsFailed == 0);
    CHECK(run.report->catalog->shelfAtlasesShown == run.report->catalog->shelfAtlases);
}

TEST_CASE("world look: the room's air (I12 step 6): dust in the key light and the pendant, cones in the lounge, the "
          "dust still under reduceMotion",
          "[gpu][world][world_look][atmosphere]") {
    ImportedRoom room("air");
    const fs::path shot = room.dir / "air.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "air", roomArguments("30", shot));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->lastView.has_value());
    // The key light's cone and the pendant's pool at the evening pack's dust, drifting.
    CHECK(run.report->lastView->atmosphereMotes == rv::scene::kDustConeMotes + rv::scene::kDustPoolMotes);
    CHECK(run.report->lastView->atmosphereCones == 0);
    CHECK_FALSE(run.report->lastView->atmosphereFrozen);
    CHECK(run.report->lastView->atmosphereGpuMilliseconds.has_value());
    REQUIRE(run.report->catalog.has_value());
    // The hero sign flickers (never in a frame-limited run's first seconds), the CRT's glow may follow its picture.
    CHECK(run.report->catalog->flickeringSigns == 1);
    CHECK(run.report->catalog->crtGlows == 1);

    // Under reduceMotion the same motes stand still.
    ImportedRoom still("air_still");
    fs::create_directories(still.data / "config");
    {
        std::ofstream config(still.data / "config" / "world.json", std::ios::binary);
        config << "{\"reduceMotion\": true}\n";
    }
    const AppRun stillRun =
        runApp(retroverseExe(), still.dir, "air_still", roomArguments("30", still.dir / "still.png"));
    requireCleanRun(stillRun);
    REQUIRE(stillRun.report->lastView.has_value());
    CHECK(stillRun.report->lastView->atmosphereMotes == run.report->lastView->atmosphereMotes);
    CHECK(stillRun.report->lastView->atmosphereFrozen);

    // The optical-era lounge (G17's room) shows its spots' cones in the air.
    ImportedRoom lounge("air_lounge", "esde/case-shelves");
    const AppRun loungeRun = runApp(retroverseExe(), lounge.dir, "lounge",
                                    {"--start-room", "room:ps2", "--exit-after-frames", "--frames", "30", "--fixed-dt",
                                     "0.016667", "--screenshot", arg(lounge.dir / "lounge.png")});
    requireCleanRun(loungeRun);
    REQUIRE(loungeRun.report->lastView.has_value());
    CHECK(loungeRun.report->lastView->atmosphereCones > 0);
    CHECK(loungeRun.report->lastView->atmosphereMotes > 0);
}

TEST_CASE("world look: the rooms' life (I12 step 8): the 90s room's fan turns, still under reduceMotion; the study's "
          "clock at 10:10",
          "[gpu][world][world_look][life]") {
    // Frames 30 and 31 of the 90s room (the clip held), with and without reduceMotion.
    const auto shoot = [](const ImportedRoom& room, const std::string& name, int frames, const std::string& start) {
        std::vector<std::string> arguments{"--start-room", start, "--exit-after-frames", "--frames",
                                           std::to_string(frames), "--fixed-dt", "0.016667", "--video-freeze-pts", "1",
                                           "--screenshot", arg(room.dir / (name + ".png"))};
        return runApp(retroverseExe(), room.dir, name, std::move(arguments));
    };
    ImportedRoom moving("life", "esde/building");
    const AppRun first = shoot(moving, "fan30", 30, "room:psx");
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    const AppRun next = shoot(moving, "fan31", 31, "room:psx");
    requireCleanRun(next);
    INFO(first.describe());
    REQUIRE(first.report->catalog.has_value());
    CHECK(first.report->catalog->spinningParts == 4);
    CHECK(first.report->catalog->clockHands == 0);
    CHECK(first.report->catalog->partMoveFrames >= 28);
    CHECK(first.report->catalog->blinkingMaterials >= 1); // the rack's LEDs
    CHECK(first.report->catalog->breathingMaterials == 1); // the lava lamp's wax
    const rv::assets::ImageData a = requireScreenshot(first, moving.dir / "fan30.png", kShotSize);
    const rv::assets::ImageData b = requireScreenshot(next, moving.dir / "fan31.png", kShotSize);
    CHECK(a.pixels != b.pixels);

    // Under reduceMotion the blades stand as described: the two frames are the same picture.
    ImportedRoom still("life_still", "esde/building");
    fs::create_directories(still.data / "config");
    {
        std::ofstream config(still.data / "config" / "world.json", std::ios::binary);
        config << "{\"reduceMotion\": true}\n";
    }
    const AppRun stillFirst = shoot(still, "still30", 30, "room:psx");
    requireCleanRun(stillFirst);
    const AppRun stillNext = shoot(still, "still31", 31, "room:psx");
    requireCleanRun(stillNext);
    REQUIRE(stillFirst.report->catalog.has_value());
    CHECK(stillFirst.report->catalog->spinningParts == 4);
    CHECK(stillFirst.report->catalog->partMoveFrames == 0);
    CHECK(requireScreenshot(stillFirst, still.dir / "still30.png", kShotSize).pixels ==
          requireScreenshot(stillNext, still.dir / "still31.png", kShotSize).pixels);

    // The study's clock: three hands, at 10:10 in a frame-limited run.
    const AppRun study = shoot(moving, "clock", 30, "area:shared:computer");
    requireCleanRun(study);
    REQUIRE(study.report->catalog.has_value());
    CHECK(study.report->catalog->clockHands == 3);
    CHECK(study.report->catalog->spinningParts == 0);
    CHECK(study.report->catalog->clockTime == "10:10:00");
}

namespace {

/// A room run with its mix captured (the null output's offline mix) and video.json's volumes `volumes`; the walker
/// walks forward from frame `walkFrom` to `walkTo` (none: it stands).
AppRun soundRun(const ImportedRoom& room, std::string_view name, std::string_view volumes, int frames, int walkFrom,
                int walkTo) {
    fs::create_directories(room.data / "config");
    {
        std::ofstream config(room.data / "config" / "video.json", std::ios::binary);
        config << "{\"audio\": " << volumes << "}\n";
    }
    std::vector<std::string> extra{"--audio", "null", "--audio-capture", arg(room.dir / (std::string(name) + ".wav"))};
    if (walkTo > walkFrom) {
        const fs::path script = room.dir / (std::string(name) + "_script.json");
        std::ofstream file(script, std::ios::binary);
        file << "[{\"fromFrame\": " << walkFrom << ", \"toFrame\": " << walkTo
             << ", \"actions\": {\"move_forward\": 1.0}}]\n";
        file.close();
        extra.insert(extra.end(), {"--input-script", arg(script)});
    }
    return runApp(retroverseExe(), room.dir, std::string(name),
                  roomArguments(std::to_string(frames), room.dir / (std::string(name) + ".png"), extra));
}

/// The capture of `run` between `from` and `to` seconds of the run, its left channel.
std::vector<float> mixWindow(const AppRun& run, double from, double to) {
    REQUIRE(run.report->audio.has_value());
    const rv::app::AudioRecord& audio = *run.report->audio;
    REQUIRE(audio.capture.has_value());
    auto wav = rv::audio::readWavFile(*audio.capture);
    REQUIRE(wav.has_value());
    const double start = audio.captureStartSeconds.value_or(0.0);
    std::vector<float> samples;
    for (std::size_t frame = 0; frame < wav->frames(); ++frame) {
        const double at = start + static_cast<double>(frame) / wav->sampleRate;
        if (at >= from && at < to) {
            samples.push_back(wav->samples[frame * 2]);
        }
    }
    return samples;
}

} // namespace

TEST_CASE("world look: the room's sound (I12 step 7): its tone in the mix, the walker's steps as it walks",
          "[gpu][world][world_look][room_sound]") {
    // The tone alone (the clips and the effects at 0): the hum, the hiss and the sign's buzz of the snes room.
    ImportedRoom room("sound");
    const AppRun tone = soundRun(room, "tone", R"({"video": 0, "ambience": 1, "effects": 0})", 60, 0, 0);
    skipWithoutOpenGl(tone);
    requireCleanRun(tone);
    {
        INFO(tone.describe());
        REQUIRE(tone.report->audio.has_value());
        const rv::app::AudioRecord& audio = *tone.report->audio;
        CHECK(audio.device == "null");
        CHECK(audio.ambienceEmitters >= 3);
        CHECK(audio.ambienceSources >= 2);
        CHECK(audio.footsteps == 0);
        // Past the loops' 300 ms fade-in: heard.
        CHECK(rv::audio::rmsOf(mixWindow(tone, 0.5, 1.0)) > 1e-4);
    }

    // The steps alone (the tone at 0): silent while the walker stands, then a step each stride.
    ImportedRoom walk("sound_steps");
    const AppRun steps = soundRun(walk, "steps", R"({"video": 0, "ambience": 0, "effects": 1})", 150, 30, 120);
    requireCleanRun(steps);
    INFO(steps.describe());
    REQUIRE(steps.report->audio.has_value());
    const rv::app::AudioRecord& audio = *steps.report->audio;
    CHECK(audio.footsteps >= 3);
    CHECK(audio.effectsTriggered == audio.footsteps);
    REQUIRE(audio.effects.size() == audio.footsteps);
    for (const rv::app::AudioEffectRecord& effect : audio.effects) {
        CHECK(effect.sound.starts_with("footstep-"));
        CHECK(effect.frame >= 30);
    }
    CHECK(rv::audio::rmsOf(mixWindow(steps, 0.0, 0.45)) == 0.0);
    // Each step is heard as an onset of the mix.
    const std::vector<float> walked = mixWindow(steps, 0.45, 2.6);
    const std::vector<double> onsets = rv::audio::onsets(walked, 48000, 0.01, 0.08);
    CHECK(onsets.size() + 1 >= audio.footsteps);
    CHECK(onsets.size() <= audio.footsteps + 1);
}