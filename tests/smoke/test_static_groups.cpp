// Static instance groups and the hover outline (I3 step 8; ADR-0011 §3, ADR-0012 §3) through rv_collection_probe's
// groups scenario (tests/collection_probe): 2,000 items (6,000 instances: shells, fronts, spines) in one
// gl::StaticInstanceGroup of 87 clusters, lit by a spot key light whose shadow map the shells are drawn into, with the
// ambient occlusion on. The same items as render items, planned every frame (--probe-path dynamic), must give the same
// pixels in every view (the whole wall, a close-up whose clusters are partly culled, an item hidden by its flag), while
// the group is planned in a few hundredths of a millisecond and written once. The outline of one highlighted item is
// probed pixel by pixel: exactly the pixels within 2 px (at 1080p) outside its silhouette, in the theme's warm white,
// nothing else changed (the anti-aliasing and the ambient occlusion untouched), the same pixels 120 frames later, unbroken
// around the item while the camera moves, within 0.1 ms of GPU time.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/core/cpu_timers.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/instance_clusters.hpp>

#include "probe_layout.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
namespace probe = rv::probe;

namespace {

std::filesystem::path probeExe() {
    return std::filesystem::path(RV_SMOKE_COLLECTION_PROBE);
}

constexpr glm::ivec2 kShotSize{1920, 1080};
/// Frames of a run: every layer is resident by about frame 90 in Debug (4,000 layers under the upload budget).
constexpr std::uint64_t kFrames = 150;
/// The groups scenario's wall and its clusters (87 of 88 hold items: the last row is short).
const probe::Grid kGrid = probe::gridFor(probe::countsOf(probe::Scenario::Groups).final);
constexpr std::size_t kOccupiedClusters = 87;
constexpr std::size_t kItems = 2000;

/// What a run of the groups scenario asks for.
struct GroupsRun {
    probe::DrawPath path = probe::DrawPath::Static;
    probe::CameraMode camera = probe::CameraMode::Wall;
    bool highlight = false;
    bool hide = false;
    std::uint64_t frames = kFrames;
    /// The window's size (the outline's GPU time is measured at 1080p); the screenshot is always kShotSize.
    std::optional<glm::ivec2> window;
};

struct ProbeShot {
    AppRun run;
    rv::assets::ImageData image;
    std::string log;
};

ProbeShot runGroups(const std::filesystem::path& caseDir, std::string_view name, const GroupsRun& request) {
    const std::filesystem::path shot = caseDir / fmt::format("{}.png", name);
    std::vector<std::string> arguments{"--probe-scenario", "groups",
                                       "--probe-path",     std::string(probe::toString(request.path)),
                                       "--probe-camera",   std::string(probe::toString(request.camera)),
                                       "--exit-after-frames",
                                       "--frames",         std::to_string(request.frames),
                                       "--fixed-dt",       "0.016667",
                                       "--screenshot",     arg(shot)};
    if (request.highlight) {
        arguments.emplace_back("--probe-highlight");
    }
    if (request.hide) {
        arguments.emplace_back("--probe-hide");
    }
    if (request.window) {
        arguments.insert(arguments.end(), {"--width", std::to_string(request.window->x), "--height",
                                           std::to_string(request.window->y)});
    }
    ProbeShot result{runApp(probeExe(), caseDir, name, std::move(arguments)), {}, {}};
    skipWithoutOpenGl(result.run);
    requireCleanRun(result.run);
    result.image = requireScreenshot(result.run, shot, kShotSize);
    const auto log = rv::core::readTextFile(result.run.outputFile);
    REQUIRE(log.has_value());
    result.log = *log;
    return result;
}

const rv::gl::FrameStats& lastView(const ProbeShot& shot) {
    REQUIRE(shot.run.report.has_value());
    REQUIRE(shot.run.report->lastView.has_value());
    return *shot.run.report->lastView;
}

const rv::app::CpuSectionRecord* findSection(const rv::app::RunReport& report, std::string_view name) {
    for (const rv::app::CpuSectionRecord& section : report.cpuSections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

std::array<int, 3> pixelAt(const rv::assets::ImageData& image, int x, int y) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) + static_cast<std::size_t>(x)) *
        static_cast<std::size_t>(image.channels);
    return {image.pixels[offset], image.pixels[offset + 1], image.pixels[offset + 2]};
}

/// Pixels whose colour differs between two images of one size, as a mask (rows top first).
std::vector<std::uint8_t> differences(const rv::assets::ImageData& a, const rv::assets::ImageData& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(a.width) * static_cast<std::size_t>(a.height), 0);
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x)] =
                pixelAt(a, x, y) != pixelAt(b, x, y) ? 1 : 0;
        }
    }
    return mask;
}

std::size_t countSet(const std::vector<std::uint8_t>& mask) {
    return static_cast<std::size_t>(std::count(mask.begin(), mask.end(), std::uint8_t{1}));
}

/// The outline's pixels: the theme's warm white, as the output stores it.
constexpr std::array<int, 3> kOutlineColor{rv::scene::kOutlineColorSrgb[0], rv::scene::kOutlineColorSrgb[1],
                                           rv::scene::kOutlineColorSrgb[2]};

/// The highlighted item's silhouette as the camera of `request` sees it at the screenshot's frame (the last), in the
/// screenshot's pixels: the convex hull of its shell's corners and its two quads' corners, projected.
struct Silhouette {
    /// Image coordinates (x right, y down; a pixel's center at +0.5), counter-clockwise as plain (x, y) numbers.
    std::vector<glm::vec2> hull;

    /// True when the center of pixel (x, y) lies inside.
    [[nodiscard]] bool covers(int x, int y) const {
        const glm::vec2 point(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
        for (std::size_t i = 0; i < hull.size(); ++i) {
            const glm::vec2 a = hull[i];
            const glm::vec2 b = hull[(i + 1) % hull.size()];
            const float cross = (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
            if (cross < 0.0f) {
                return false;
            }
        }
        return true;
    }
};

Silhouette silhouetteOf(const GroupsRun& request) {
    const rv::scene::CameraSetup setup = probe::cameraAt(kGrid, request.camera, request.frames - 1);
    rv::scene::Camera camera;
    camera.setLens(setup.lens);
    camera.setPose(setup.position, rv::core::degreesToRadians(setup.yawDeg),
                   rv::core::degreesToRadians(setup.pitchDeg));
    camera.setAspect(static_cast<float>(kShotSize.x) / static_cast<float>(kShotSize.y));
    const glm::mat4 viewProjection = camera.viewProjection();
    const std::size_t item = probe::highlightItem(kGrid);
    std::vector<glm::vec3> corners;
    const glm::vec3 low = probe::shellMin(kGrid, item);
    const glm::vec3 high = probe::shellMax(kGrid, item);
    for (int corner = 0; corner < 8; ++corner) {
        corners.emplace_back((corner & 1) != 0 ? high.x : low.x, (corner & 2) != 0 ? high.y : low.y,
                             (corner & 4) != 0 ? high.z : low.z);
    }
    for (const auto& [center, size] : {std::pair{probe::frontCenter(kGrid, item), probe::kFrontSize},
                                       std::pair{probe::spineCenter(kGrid, item), probe::kSpineSize}}) {
        for (int corner = 0; corner < 4; ++corner) {
            corners.push_back(center + glm::vec3(((corner & 1) != 0 ? 0.5f : -0.5f) * size.x,
                                                 ((corner & 2) != 0 ? 0.5f : -0.5f) * size.y, 0.0f));
        }
    }
    std::vector<glm::vec2> points;
    for (const glm::vec3& corner : corners) {
        const glm::vec4 clip = viewProjection * glm::vec4(corner, 1.0f);
        REQUIRE(clip.w > 0.0f);
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        // Window coordinates (y up), then image rows (top first).
        const glm::vec2 window = (ndc * 0.5f + 0.5f) * glm::vec2(kShotSize);
        points.emplace_back(window.x, static_cast<float>(kShotSize.y) - window.y);
    }
    // Andrew's monotone chain: counter-clockwise in (x, y), so `covers` finds the inside on the positive side of every
    // edge.
    std::sort(points.begin(), points.end(),
              [](const glm::vec2& a, const glm::vec2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    const auto cross = [](const glm::vec2& o, const glm::vec2& a, const glm::vec2& b) {
        return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
    };
    std::vector<glm::vec2> hull;
    for (int pass = 0; pass < 2; ++pass) {
        const std::size_t start = hull.size();
        for (std::size_t i = 0; i < points.size(); ++i) {
            const glm::vec2& p = pass == 0 ? points[i] : points[points.size() - 1 - i];
            while (hull.size() >= start + 2 && cross(hull[hull.size() - 2], hull.back(), p) <= 0.0f) {
                hull.pop_back();
            }
            hull.push_back(p);
        }
        hull.pop_back();
    }
    return Silhouette{hull};
}

/// The outline the edge pass draws around `silhouette` at kShotSize: every pixel outside it with a pixel inside within
/// the outline's width (scene::outlineCovers), the neighbors inside the image only.
std::vector<std::uint8_t> expectedOutline(const Silhouette& silhouette) {
    const float width = rv::scene::outlineWidthPixels(kShotSize.y);
    const int reach = rv::scene::outlineReach(width);
    std::vector<std::uint8_t> inside(static_cast<std::size_t>(kShotSize.x) * static_cast<std::size_t>(kShotSize.y), 0);
    for (int y = 0; y < kShotSize.y; ++y) {
        for (int x = 0; x < kShotSize.x; ++x) {
            inside[static_cast<std::size_t>(y * kShotSize.x + x)] = silhouette.covers(x, y) ? 1 : 0;
        }
    }
    std::vector<std::uint8_t> outline(inside.size(), 0);
    for (int y = 0; y < kShotSize.y; ++y) {
        for (int x = 0; x < kShotSize.x; ++x) {
            if (inside[static_cast<std::size_t>(y * kShotSize.x + x)] != 0) {
                continue;
            }
            for (int dy = -reach; dy <= reach && outline[static_cast<std::size_t>(y * kShotSize.x + x)] == 0; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= kShotSize.x || ny >= kShotSize.y ||
                        !rv::scene::outlineCovers(dx, dy, width)) {
                        continue;
                    }
                    if (inside[static_cast<std::size_t>(ny * kShotSize.x + nx)] != 0) {
                        outline[static_cast<std::size_t>(y * kShotSize.x + x)] = 1;
                        break;
                    }
                }
            }
        }
    }
    return outline;
}

/// Pixels set in exactly one of two masks.
std::size_t mismatches(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        count += a[i] != b[i] ? 1 : 0;
    }
    return count;
}

/// Pixels of an expected outline whose image pixel is not exactly the outline's colour, and pixels of the outline's
/// colour within `margin` of the expected outline's bounding box that the outline does not hold.
struct OutlineCheck {
    std::size_t expected = 0;
    std::size_t missing = 0;
    std::size_t stray = 0;
    /// Missing pixels without an outline pixel among their 8 neighbors: a break in the ring (a pixel whose center lies on
    /// the silhouette's edge moves the ring by one pixel there, which is no break).
    std::size_t gaps = 0;
};

OutlineCheck checkOutline(const rv::assets::ImageData& image, const std::vector<std::uint8_t>& expected, int margin) {
    OutlineCheck check;
    glm::ivec2 low(kShotSize);
    glm::ivec2 high(-1);
    for (int y = 0; y < kShotSize.y; ++y) {
        for (int x = 0; x < kShotSize.x; ++x) {
            if (expected[static_cast<std::size_t>(y * kShotSize.x + x)] == 0) {
                continue;
            }
            ++check.expected;
            low = glm::min(low, glm::ivec2(x, y));
            high = glm::max(high, glm::ivec2(x, y));
            if (pixelAt(image, x, y) == kOutlineColor) {
                continue;
            }
            ++check.missing;
            bool neighbor = false;
            for (int dy = -1; dy <= 1 && !neighbor; ++dy) {
                for (int dx = -1; dx <= 1 && !neighbor; ++dx) {
                    const int nx = x + dx;
                    const int ny = y + dy;
                    neighbor = nx >= 0 && ny >= 0 && nx < kShotSize.x && ny < kShotSize.y &&
                               pixelAt(image, nx, ny) == kOutlineColor;
                }
            }
            check.gaps += neighbor ? 0 : 1;
        }
    }
    for (int y = std::max(0, low.y - margin); y <= std::min(kShotSize.y - 1, high.y + margin); ++y) {
        for (int x = std::max(0, low.x - margin); x <= std::min(kShotSize.x - 1, high.x + margin); ++x) {
            if (expected[static_cast<std::size_t>(y * kShotSize.x + x)] == 0 && pixelAt(image, x, y) == kOutlineColor) {
                ++check.stray;
            }
        }
    }
    return check;
}

/// Pixels whose center lies within a fraction of a pixel of the silhouette's edge may fall either way between the
/// CPU's projection and the GPU's rasterization: a few of the outline's ~1,200 pixels.
std::size_t edgeTolerance(std::size_t expected) {
    return std::max<std::size_t>(4, expected / 100);
}

} // namespace

TEST_CASE("static groups/2,000 items drawn like render items: planned once, culled per cluster, written once",
          "[gpu][static_groups]") {
    const std::filesystem::path caseDir = freshCaseDir("static_groups_paths");
    const ProbeShot wallStatic = runGroups(caseDir, "wall_static", {});
    const ProbeShot wallDynamic = runGroups(caseDir, "wall_dynamic", {probe::DrawPath::Dynamic});
    const ProbeShot closeStatic = runGroups(caseDir, "close_static", {probe::DrawPath::Static, probe::CameraMode::Close});
    const ProbeShot closeDynamic =
        runGroups(caseDir, "close_dynamic", {probe::DrawPath::Dynamic, probe::CameraMode::Close});
    const ProbeShot hiddenStatic =
        runGroups(caseDir, "hidden_static", {probe::DrawPath::Static, probe::CameraMode::Close, false, true});
    const ProbeShot hiddenDynamic =
        runGroups(caseDir, "hidden_dynamic", {probe::DrawPath::Dynamic, probe::CameraMode::Close, false, true});

    // The same pixels, shadows and ambient occlusion included: the group draws what the planner draws.
    CHECK(countSet(differences(wallStatic.image, wallDynamic.image)) == 0);
    CHECK(countSet(differences(closeStatic.image, closeDynamic.image)) == 0);
    CHECK(countSet(differences(hiddenStatic.image, hiddenDynamic.image)) == 0);
    // The hidden item is gone (its flag hides its record in every pass, the shadow map's included).
    CHECK(countSet(differences(closeStatic.image, hiddenStatic.image)) > 1000);

    // The whole wall: one draw per batch (the backdrop's, then the shells', the fronts' and the spines'), every pass.
    const rv::gl::FrameStats& wall = lastView(wallStatic);
    CHECK(wall.items == 1);
    CHECK(wall.staticGroups == 1);
    CHECK(wall.staticBatches == 3);
    CHECK(wall.batches == 4);
    CHECK(wall.clusters == kOccupiedClusters);
    CHECK(wall.culledClusters == 0);
    CHECK(wall.staticDrawCalls == 3);
    CHECK(wall.staticInstances == 3 * kItems);
    CHECK(wall.drawCalls == 4);
    CHECK(wall.instances == 3 * kItems + 1);
    CHECK(wall.depthDrawCalls == 4);
    // The shells cast (one draw), their labels and spines do not, nor does the backdrop.
    CHECK(wall.shadowDrawCalls == 1);
    CHECK(wall.shadowInstances == kItems);
    const rv::gl::FrameStats& wallItems = lastView(wallDynamic);
    CHECK(wallItems.items == 3 * kItems + 1);
    CHECK(wallItems.staticGroups == 0);
    CHECK(wallItems.drawCalls == wall.drawCalls);
    CHECK(wallItems.instances == wall.instances);
    CHECK(wallItems.triangles == wall.triangles);
    CHECK(wallItems.shadowInstances == wall.shadowInstances);

    // The close-up: whole clusters culled, the visible ones merged into a few ranges per batch, a superset of the items
    // the planner keeps one by one.
    const rv::gl::FrameStats& close = lastView(closeStatic);
    CHECK(close.culledClusters > kOccupiedClusters / 2);
    CHECK(close.culledClusters < kOccupiedClusters);
    CHECK(close.staticDrawCalls > 3);
    CHECK(close.staticDrawCalls <= 3 * (kOccupiedClusters - close.culledClusters));
    CHECK(close.staticInstances < 3 * kItems);
    const rv::gl::FrameStats& closeItems = lastView(closeDynamic);
    CHECK(closeItems.culledItems > 0);
    CHECK(closeItems.instances - 1 <= close.staticInstances);
    UNSCOPED_INFO(fmt::format("close-up: {} of {} clusters culled, {} static draw calls of {} instances; the planner "
                              "keeps {} of {} items",
                              close.culledClusters, close.clusters, close.staticDrawCalls, close.staticInstances,
                              closeItems.instances, closeItems.items));

    // Planning: the CPU section of every frame of the static runs (the roadmap's 0.3 ms), none for render items; the
    // group's buffer written once, at its creation, and never again in a still room.
    for (const ProbeShot* shot : {&wallStatic, &closeStatic, &hiddenStatic}) {
        const rv::app::CpuSectionRecord* planning =
            findSection(*shot->run.report, rv::core::cpu_section::kCollectionPlanning);
        REQUIRE(planning != nullptr);
        CHECK(planning->times.count == kFrames);
        CHECK(planning->times.p50Ms <= 0.3);
        UNSCOPED_INFO(fmt::format("{}: collectionPlanning p50 {:.3f} ms, p99 {:.3f} ms ({})", shot->run.name,
                                  planning->times.p50Ms, planning->times.p99Ms, shot->run.report->buildType));
        CHECK(shot->log.find(fmt::format("Static instance group: {} bytes uploaded at its creation, 0 bytes written "
                                         "since",
                                         3 * kItems * sizeof(rv::scene::InstanceData))) != std::string::npos);
    }
    for (const ProbeShot* shot : {&wallDynamic, &closeDynamic, &hiddenDynamic}) {
        CHECK(findSection(*shot->run.report, rv::core::cpu_section::kCollectionPlanning) == nullptr);
    }
}

TEST_CASE("static groups/the outline around a highlighted item: its pixels exactly, nothing else changed",
          "[gpu][static_groups][outline]") {
    const std::filesystem::path caseDir = freshCaseDir("static_groups_outline");
    const GroupsRun plain{probe::DrawPath::Static, probe::CameraMode::Close};
    GroupsRun highlighted = plain;
    highlighted.highlight = true;
    GroupsRun itemsHighlighted = highlighted;
    itemsHighlighted.path = probe::DrawPath::Dynamic;
    GroupsRun later = highlighted;
    later.frames = kFrames + 120;
    const ProbeShot without = runGroups(caseDir, "without", plain);
    const ProbeShot with = runGroups(caseDir, "with", highlighted);
    const ProbeShot withItems = runGroups(caseDir, "with_items", itemsHighlighted);
    const ProbeShot still = runGroups(caseDir, "still_120_frames_later", later);

    // What changed is the outline: exactly the outline's colour, written after the anti-aliasing, on exactly the pixels
    // within 2 px outside the item's silhouette. Every other pixel, the shadows and the ambient occlusion around the
    // item included, is as without the highlight.
    const std::vector<std::uint8_t> changed = differences(without.image, with.image);
    const std::vector<std::uint8_t> expected = expectedOutline(silhouetteOf(highlighted));
    const std::size_t outlinePixels = countSet(expected);
    CHECK(outlinePixels > 800);
    CHECK(mismatches(changed, expected) <= edgeTolerance(outlinePixels));
    std::size_t otherColour = 0;
    for (int y = 0; y < kShotSize.y; ++y) {
        for (int x = 0; x < kShotSize.x; ++x) {
            if (changed[static_cast<std::size_t>(y * kShotSize.x + x)] != 0 && pixelAt(with.image, x, y) != kOutlineColor) {
                ++otherColour;
            }
        }
    }
    CHECK(otherColour == 0);
    UNSCOPED_INFO(fmt::format("{} pixels changed, {} expected, {} apart", countSet(changed), outlinePixels,
                              mismatches(changed, expected)));

    // 2 px wide at 1080p: the row through the item's middle crosses two outline pixels on each side.
    const Silhouette silhouette = silhouetteOf(highlighted);
    float top = 1e9f;
    float bottom = -1e9f;
    for (const glm::vec2& point : silhouette.hull) {
        top = std::min(top, point.y);
        bottom = std::max(bottom, point.y);
    }
    const int row = static_cast<int>((top + bottom) * 0.5f);
    std::vector<int> runs;
    int length = 0;
    for (int x = 0; x < kShotSize.x; ++x) {
        if (changed[static_cast<std::size_t>(row * kShotSize.x + x)] != 0) {
            ++length;
        } else if (length > 0) {
            runs.push_back(length);
            length = 0;
        }
    }
    CHECK(runs == std::vector<int>{2, 2});

    // The render items' highlight draws the same outline (the flag is the same record field on both paths).
    CHECK(countSet(differences(with.image, withItems.image)) == 0);
    // A still camera, 120 frames later: the very same pixels.
    CHECK(countSet(differences(with.image, still.image)) == 0);

    // The report: three instances marked (the item's shell, front and spine), the outline timed; the group written
    // once more, the 16 bytes of each record's attributes.
    CHECK(lastView(with).outlinedInstances == 3);
    CHECK(lastView(with).outlineGpuMilliseconds.has_value());
    CHECK_FALSE(lastView(without).outlineGpuMilliseconds.has_value());
    CHECK(with.log.find("48 bytes written since") != std::string::npos);
}

TEST_CASE("static groups/the outline stays unbroken around the item while the camera moves", "[gpu][static_groups][outline]") {
    const std::filesystem::path caseDir = freshCaseDir("static_groups_moving");
    // The moving camera's frames 150 to 171 (its eye moves 1.3 mm right, 0.7 mm down and 2.9 mm closer a frame): the
    // item's edges cross pixels at sub-pixel steps, and every frame's outline holds every pixel it should.
    for (const std::uint64_t frames : {kFrames, kFrames + 7, kFrames + 14, kFrames + 21}) {
        GroupsRun moving{probe::DrawPath::Static, probe::CameraMode::Moving, true};
        moving.frames = frames;
        const ProbeShot shot = runGroups(caseDir, fmt::format("moving_{}", frames), moving);
        const std::vector<std::uint8_t> expected = expectedOutline(silhouetteOf(moving));
        const OutlineCheck check = checkOutline(shot.image, expected, 6);
        INFO("frame " << frames - 1 << ": " << check.expected << " outline pixels, " << check.missing << " missing ("
                      << check.gaps << " gaps), " << check.stray << " stray");
        CHECK(check.expected > 800);
        CHECK(check.gaps == 0);
        CHECK(check.missing <= edgeTolerance(check.expected));
        CHECK(check.stray <= edgeTolerance(check.expected));
    }
}

TEST_CASE("static groups/the outline's GPU time at 1080p", "[gpu][static_groups][outline]") {
    // The budget is 0.1 ms (docs/roadmap.md): the median over the frames that drew it, the window at 1920 x 1080.
    const std::filesystem::path caseDir = freshCaseDir("static_groups_outline_time");
    GroupsRun timed{probe::DrawPath::Static, probe::CameraMode::Close, true};
    timed.window = glm::ivec2(1920, 1080);
    const ProbeShot shot = runGroups(caseDir, "outline_1080p", timed);
    std::smatch match;
    REQUIRE(std::regex_search(shot.log, match,
                              std::regex(R"(Outline GPU time over (\d+) frame\(s\): median ([0-9.]+) ms, max ([0-9.]+) ms)")));
    const int frames = std::stoi(match[1].str());
    const double median = std::stod(match[2].str());
    UNSCOPED_INFO(fmt::format("outline GPU time over {} frames: median {} ms, max {} ms", frames, median,
                              match[3].str()));
    CHECK(frames > 100);
    CHECK(median <= 0.1);
}
