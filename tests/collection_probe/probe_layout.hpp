#pragma once

// What rv_collection_probe draws, shared with the GPU smoke test that checks its screenshots (tests/smoke/
// test_collection_arrays.cpp): the scenarios, where each item stands, the camera that frames them and the picture of
// every layer. GPU-free and header only (glm, rv_scene's camera setup, the arrays' colors of rv_render_uniforms).
//
// An item is a test stand-in for a cartridge: a grey shell box with two quads on its face, the front label (left) and
// the spine (right; drawn facing the camera too, so that both classes can be probed in one picture). Every layer is a
// solid color, exact in BC1 (both endpoints one RGB565 value) and in RGBA8 alike, unique among all the layers of a run
// and never one of the pending pictures, so that a pixel at a quad's center names the layer it shows.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <rv/render/texture_array_layout.hpp>
#include <rv/scene/scene_description.hpp>

namespace rv::probe {

/// What a run of the probe exercises.
enum class Scenario : std::uint8_t {
    /// 2,049 items: each class needs two arrays (2,048 layers and 64), so the fronts and the spines draw in two
    /// batches each, plus the shells' one.
    Chunks,
    /// 63 items in arrays of 64 layers, every layer resident, then 128 more at kGrowthFrame: both arrays grow to 192
    /// layers by a GPU copy, the first 63 pictures kept without being uploaded again.
    Growth,
    /// 2,000 items (I3 step 8) as a static instance group (or, with --probe-path dynamic, as render items planned every
    /// frame, to compare): each half row of the wall one cluster, lit by a spot key light that casts the shells'
    /// shadows on the backdrop, with ambient occlusion; the item's layers are named in its records from the start.
    Groups,
};

[[nodiscard]] constexpr std::string_view toString(Scenario scenario) noexcept {
    switch (scenario) {
    case Scenario::Chunks:
        return "chunks";
    case Scenario::Growth:
        return "growth";
    case Scenario::Groups:
        return "groups";
    }
    return "chunks";
}

[[nodiscard]] constexpr std::optional<Scenario> scenarioFromString(std::string_view name) noexcept {
    if (name == "chunks") {
        return Scenario::Chunks;
    }
    if (name == "growth") {
        return Scenario::Growth;
    }
    if (name == "groups") {
        return Scenario::Groups;
    }
    return std::nullopt;
}

/// How the groups scenario draws its items.
enum class DrawPath : std::uint8_t {
    /// One gl::StaticInstanceGroup, planned once (RenderView::staticGroups).
    Static,
    /// Render items, planned by the scene::BatchPlanner every frame (RenderView::items).
    Dynamic,
};

[[nodiscard]] constexpr std::string_view toString(DrawPath path) noexcept {
    return path == DrawPath::Static ? "static" : "dynamic";
}

/// Where the probe's camera stands.
enum class CameraMode : std::uint8_t {
    /// cameraFor(): the whole wall.
    Wall,
    /// closeCamera(): 2 m in front of the highlighted item, slightly off its axis, about 9 x 9 items in view.
    Close,
    /// closeCamera() moving a little every frame (movingCamera()), still aimed at the highlighted item.
    Moving,
};

[[nodiscard]] constexpr std::string_view toString(CameraMode mode) noexcept {
    switch (mode) {
    case CameraMode::Wall:
        return "wall";
    case CameraMode::Close:
        return "close";
    case CameraMode::Moving:
        return "moving";
    }
    return "wall";
}

[[nodiscard]] constexpr std::optional<CameraMode> cameraModeFromString(std::string_view name) noexcept {
    if (name == "wall") {
        return CameraMode::Wall;
    }
    if (name == "close") {
        return CameraMode::Close;
    }
    if (name == "moving") {
        return CameraMode::Moving;
    }
    return std::nullopt;
}

/// The frame at which the growth scenario adds its items (the first frame from it on at which every layer of the first
/// items is resident).
inline constexpr std::uint64_t kGrowthFrame = 20;

/// Items at the start and at the end of a scenario.
struct ScenarioCounts {
    std::size_t initial = 0;
    std::size_t final = 0;
};

[[nodiscard]] constexpr ScenarioCounts countsOf(Scenario scenario) noexcept {
    switch (scenario) {
    case Scenario::Chunks:
        return ScenarioCounts{2049, 2049};
    case Scenario::Growth:
        return ScenarioCounts{63, 191};
    case Scenario::Groups:
        return ScenarioCounts{2000, 2000};
    }
    return ScenarioCounts{};
}

/// Cell of one item in the wall of items, meters; the wall stands in the plane z = 0, centered on the origin.
inline constexpr glm::vec2 kCellSize{0.26f, 0.15f};
/// The shell box, centered on its cell, its face at z = kShellSize.z / 2.
inline constexpr glm::vec3 kShellSize{0.24f, 0.13f, 0.03f};
/// The quads, kQuadOffset in front of the shell's face.
inline constexpr glm::vec2 kFrontSize{0.16f, 0.117f};
inline constexpr glm::vec2 kSpineSize{0.06f, 0.117f};
inline constexpr float kQuadOffset = 0.002f;
/// Quad centers relative to the cell's center (x).
inline constexpr float kFrontCenterX = -0.04f;
inline constexpr float kSpineCenterX = 0.08f;
/// Horizontal field of view of the camera; the screenshots have the window's 16:9 aspect.
inline constexpr float kHorizontalFovDeg = 30.0f;
inline constexpr float kAspect = 16.0f / 9.0f;

/// Columns and rows of the wall of `items` (all a scenario will hold), so that the wall's aspect is about 16:9.
struct Grid {
    int columns = 1;
    int rows = 1;
};

[[nodiscard]] inline Grid gridFor(std::size_t items) noexcept {
    const double cellAspect = static_cast<double>(kCellSize.x) / static_cast<double>(kCellSize.y);
    const double count = static_cast<double>(items > 0 ? items : 1);
    Grid grid;
    grid.columns = static_cast<int>(std::ceil(std::sqrt(count * static_cast<double>(kAspect) / cellAspect)));
    grid.rows = static_cast<int>(std::ceil(count / grid.columns));
    return grid;
}

/// The center of item `index`'s cell: row 0 at the top, columns from the left.
[[nodiscard]] inline glm::vec3 cellCenter(const Grid& grid, std::size_t index) noexcept {
    const int column = static_cast<int>(index % static_cast<std::size_t>(grid.columns));
    const int row = static_cast<int>(index / static_cast<std::size_t>(grid.columns));
    const float width = static_cast<float>(grid.columns) * kCellSize.x;
    const float height = static_cast<float>(grid.rows) * kCellSize.y;
    return {-width / 2.0f + (static_cast<float>(column) + 0.5f) * kCellSize.x,
            height / 2.0f - (static_cast<float>(row) + 0.5f) * kCellSize.y, 0.0f};
}

[[nodiscard]] inline glm::vec3 frontCenter(const Grid& grid, std::size_t index) noexcept {
    return cellCenter(grid, index) + glm::vec3(kFrontCenterX, 0.0f, kShellSize.z / 2.0f + kQuadOffset);
}

[[nodiscard]] inline glm::vec3 spineCenter(const Grid& grid, std::size_t index) noexcept {
    return cellCenter(grid, index) + glm::vec3(kSpineCenterX, 0.0f, kShellSize.z / 2.0f + kQuadOffset);
}

/// A camera on +Z looking at the wall (yaw 0 looks at -Z), far enough for the whole wall with a margin.
[[nodiscard]] inline scene::CameraSetup cameraFor(const Grid& grid) noexcept {
    const float halfWidth = static_cast<float>(grid.columns) * kCellSize.x / 2.0f * 1.04f;
    const float halfHeight = static_cast<float>(grid.rows) * kCellSize.y / 2.0f * 1.04f;
    const float tanHalfH = std::tan(glm::radians(kHorizontalFovDeg * 0.5f));
    const float tanHalfV = tanHalfH / kAspect;
    const float distance = std::fmax(halfWidth / tanHalfH, halfHeight / tanHalfV);
    scene::CameraSetup camera;
    camera.position = glm::vec3(0.0f, 0.0f, distance);
    camera.yawDeg = 0.0f;
    camera.pitchDeg = 0.0f;
    camera.lens.horizontalFovDeg = kHorizontalFovDeg;
    camera.lens.nearPlane = 1.0f;
    camera.lens.farPlane = distance * 2.0f + 10.0f;
    return camera;
}

/// The color of picture `k` (layer pictures are numbered 2 * item + 0 for fronts, + 1 for spines): an RGB565 value with
/// red and blue in [8, 31] and green in [16, 63] (none dark), scrambled so that neighbors differ, decoded by bit
/// replication. Distinct for k < 27,648.
[[nodiscard]] inline gl::Rgb8 pictureColor(std::size_t k) noexcept {
    constexpr std::size_t kReds = 24;
    constexpr std::size_t kGreens = 48;
    constexpr std::size_t kBlues = 24;
    constexpr std::size_t kColors = kReds * kGreens * kBlues;
    const std::size_t scrambled = (k * 7919u) % kColors; // 7919 is prime to 27,648 = 2^10 * 27
    const auto r = static_cast<std::uint16_t>(8 + scrambled % kReds);
    const auto g = static_cast<std::uint16_t>(16 + (scrambled / kReds) % kGreens);
    const auto b = static_cast<std::uint16_t>(8 + scrambled / (kReds * kGreens));
    return gl::fromRgb565(static_cast<std::uint16_t>((r << 11) | (g << 5) | b));
}

[[nodiscard]] inline gl::Rgb8 frontColor(std::size_t item) noexcept {
    return pictureColor(2 * item);
}

[[nodiscard]] inline gl::Rgb8 spineColor(std::size_t item) noexcept {
    return pictureColor(2 * item + 1);
}

/// The pending pictures (layer 0): magenta fronts and cyan spines, outside pictureColor's range (red or green 0).
inline constexpr gl::Rgb8 kPendingFront{255, 0, 255};
inline constexpr gl::Rgb8 kPendingSpine{0, 255, 255};

// --- The groups scenario (I3 step 8) ---------------------------------------------------------------------------------

/// Items per cluster along a row: a cluster is half a row of the wall (like a shelf level of a module).
inline constexpr int kClusterColumns = 23;

[[nodiscard]] inline int clustersPerRow(const Grid& grid) noexcept {
    return (grid.columns + kClusterColumns - 1) / kClusterColumns;
}

[[nodiscard]] inline std::uint32_t clusterCount(const Grid& grid) noexcept {
    return static_cast<std::uint32_t>(clustersPerRow(grid) * grid.rows);
}

/// The cluster of item `index`: its row's clusters left to right, the rows top to bottom (neighbors on the wall are
/// neighbors in the numbering).
[[nodiscard]] inline std::uint32_t clusterOf(const Grid& grid, std::size_t index) noexcept {
    const int column = static_cast<int>(index % static_cast<std::size_t>(grid.columns));
    const int row = static_cast<int>(index / static_cast<std::size_t>(grid.columns));
    return static_cast<std::uint32_t>(row * clustersPerRow(grid) + column / kClusterColumns);
}

/// The item --probe-highlight outlines (from kHighlightFrame on) and the close cameras aim at: near the wall's center.
[[nodiscard]] inline std::size_t highlightItem(const Grid& grid) noexcept {
    return static_cast<std::size_t>((grid.rows / 2) * grid.columns + grid.columns / 2 - 3);
}

/// The item --probe-hide hides (from the start): the highlighted item's right neighbor.
[[nodiscard]] inline std::size_t hiddenItem(const Grid& grid) noexcept {
    return highlightItem(grid) + 1;
}

/// The frame from which the highlighted item carries its flag: the frames before it write no instance byte.
inline constexpr std::uint64_t kHighlightFrame = 10;

/// The shell box of item `index`: its world corners' bounds.
[[nodiscard]] inline glm::vec3 shellMin(const Grid& grid, std::size_t index) noexcept {
    return cellCenter(grid, index) - kShellSize * 0.5f;
}

[[nodiscard]] inline glm::vec3 shellMax(const Grid& grid, std::size_t index) noexcept {
    return cellCenter(grid, index) + kShellSize * 0.5f;
}

/// A camera at `eye` aimed at `target`, with the close cameras' lens (60 degrees across).
[[nodiscard]] inline scene::CameraSetup aimedCamera(const glm::vec3& eye, const glm::vec3& target) noexcept {
    const glm::vec3 offset = target - eye;
    const float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    scene::CameraSetup camera;
    camera.position = eye;
    // Camera conventions: yaw 0 looks at -Z, positive yaw turns towards +X, positive pitch looks up.
    camera.yawDeg = glm::degrees(std::atan2(offset.x, -offset.z));
    camera.pitchDeg = glm::degrees(std::asin(offset.y / distance));
    camera.lens.horizontalFovDeg = 60.0f;
    camera.lens.nearPlane = 0.05f;
    camera.lens.farPlane = 60.0f;
    return camera;
}

/// The close camera: 2 m in front of the highlighted item, 0.35 m to its right and 0.25 m above (so the shells' side
/// and top faces show), aimed at its center.
[[nodiscard]] inline scene::CameraSetup closeCamera(const Grid& grid) noexcept {
    const glm::vec3 target = cellCenter(grid, highlightItem(grid));
    return aimedCamera(target + glm::vec3(0.35f, 0.25f, 2.0f), target);
}

/// The moving camera at frame `frame`: the close camera's eye moving 1.3 mm right, 0.7 mm down and 2.9 mm closer per
/// frame (sub-pixel steps of the item's edges, about 0.2 m/s at 60 fps), still aimed at the item.
[[nodiscard]] inline scene::CameraSetup movingCamera(const Grid& grid, std::uint64_t frame) noexcept {
    const glm::vec3 target = cellCenter(grid, highlightItem(grid));
    const float t = static_cast<float>(frame);
    return aimedCamera(target + glm::vec3(0.35f + 0.0013f * t, 0.25f - 0.0007f * t, 2.0f - 0.0029f * t), target);
}

/// The camera of `mode` at frame `frame`.
[[nodiscard]] inline scene::CameraSetup cameraAt(const Grid& grid, CameraMode mode, std::uint64_t frame) noexcept {
    switch (mode) {
    case CameraMode::Wall:
        return cameraFor(grid);
    case CameraMode::Close:
        return closeCamera(grid);
    case CameraMode::Moving:
        return movingCamera(grid, frame);
    }
    return cameraFor(grid);
}

} // namespace rv::probe
