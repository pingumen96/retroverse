// The generated room's look (I3 step 16, style §4.1, §4.2, §8.1, §26.2): pixel probes on screenshots of the snes-room
// fixture's room, in both lighting profiles (world.json's "lightingProfile"; switching one moves nothing):
//   - from the doorway: every collection module's eye-height band (1.1 to 1.5 m) at least a third of the hero wall's
//     luminance (its modules over their height and the wall behind the hero corner); the upper walls (2.6 to 3.0 m)
//     darker than the modules' bands; the sign and the CRT the most saturated regions of the view, the collection's
//     items aside (the 90th percentile of their chroma above every other tile's);
//   - close to a display case (0.9 m in front of its first face-out item): the covers behind the glass keep their darks
//     (no haze: the glass adds at most a few steps to the darkest pixels of the items behind it) and their contrast.
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
#include <rv/core/math.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/collection_layout.hpp>
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

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The snes-room fixture recorded as the library of <caseDir>/data and imported with retroverse-cli.
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedRoom(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("world_look/") + std::string(name))), data(dir / "data") {
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
    std::vector<std::string> arguments{"--exit-after-frames", "--frames", std::string(frames), "--fixed-dt", "0.016667",
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
    WARN("look probes (" << profile << "): hero wall " << *heroLuminance << ", eye bands " << bandMean << ", upper walls "
                         << *upperLuminance << "; chroma sign " << signChroma << ", CRT " << crtChroma
                         << ", other tiles at most " << otherChroma);
}

/// Close to a display case: the covers behind its glass keep their darks and their contrast.
void checkDisplayGlass(const ShownRoom& shown, const rv::assets::ImageData& image, const Viewpoint& view,
                       std::string_view profile) {
    INFO("profile " << profile);
    std::size_t probed = 0;
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
        CHECK(dark <= 70.0);
        CHECK(bright - dark >= 60.0);
    }
    CHECK(probed >= 3);
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
        CHECK(run.report->world.lights <= 16);
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
