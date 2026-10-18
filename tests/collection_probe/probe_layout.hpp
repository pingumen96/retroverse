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
};

[[nodiscard]] constexpr std::string_view toString(Scenario scenario) noexcept {
    return scenario == Scenario::Chunks ? "chunks" : "growth";
}

[[nodiscard]] constexpr std::optional<Scenario> scenarioFromString(std::string_view name) noexcept {
    if (name == "chunks") {
        return Scenario::Chunks;
    }
    if (name == "growth") {
        return Scenario::Growth;
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
    return scenario == Scenario::Chunks ? ScenarioCounts{2049, 2049} : ScenarioCounts{63, 191};
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

} // namespace rv::probe
