#pragma once

// Hand-built collection shelves for the picking and interaction tests (I3 step 7): modules of the figures of I3 step 1b
// (docs/i3/step-1b.md: 0.9 x 1.775 x 0.24 m, a closed base to 0.355 m, boards whose tops stand at 0.375 m and every 20
// cm above, spine-out slots 2.0 x 12.5 x 8.5 cm at a 2.4 cm pitch, the first 3 cm from the side, 1 cm behind the boards'
// front edge; a face-out level of 6 slots 12.5 x 8.5 x 2.0 cm for the hero band), as the pick targets, occluders
// (boards, sides, base, back panel) and D-pad navigation runs step 10a's collectionLayout will give the app. The
// geometry is built here from those figures, not from the prefabs, so that the tests read as the measurements they
// check.

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/slot_navigation.hpp>

namespace rv::test {

/// Module figures, meters, in the module's frame: its front faces +Z, the floor at y = 0, its middle at x = z = 0.
namespace shelf {
inline constexpr float kWidth = 0.9f;
inline constexpr float kHeight = 1.775f;
inline constexpr float kDepth = 0.24f;
inline constexpr float kSideThickness = 0.018f;
inline constexpr float kBoardThickness = 0.02f;
inline constexpr float kBaseTop = 0.355f;
inline constexpr float kFirstBoardTop = 0.375f;
inline constexpr float kLevelPitch = 0.2f;
inline constexpr float kBackThickness = 0.012f;
/// Spine-out slots.
inline constexpr float kSpineWidth = 0.020f;
inline constexpr float kSpineHeight = 0.125f;
inline constexpr float kSpineDepth = 0.085f;
inline constexpr float kSpinePitch = 0.024f;
inline constexpr float kSlotMargin = 0.03f;
inline constexpr float kFrontInset = 0.01f;
/// Face-out slots (the hero band).
inline constexpr float kFaceWidth = 0.125f;
inline constexpr float kFaceHeight = 0.085f;
inline constexpr float kFaceDepth = 0.020f;
inline constexpr float kFaceGap = 0.006f;
inline constexpr float kFaceMargin = 0.025f;
inline constexpr int kFaceSlots = 6;
/// Modules along a wall: their middles 1 m apart.
inline constexpr float kModuleSpacing = 1.0f;
} // namespace shelf

struct ShelfModuleSpec {
    int levels = 7;
    int slotsPerLevel = 33;
    /// Levels that hold shelf dressing instead of items (every slot without an item).
    std::vector<int> dressedLevels;
    /// (level, slot) pairs left empty.
    std::vector<std::pair<int, int>> emptySlots;
    /// A level of face-out slots (shelf::kFaceSlots of them) instead of spine-out ones.
    std::optional<int> faceOutLevel;
};

struct SyntheticItem {
    world::PickId id = world::kNoPick;
    int module = 0;
    int level = 0;
    int slot = 0;
    bool faceOut = false;
    /// The slot's frame (local to world) and the item's box in it, its visible face towards the frame's +Z.
    glm::mat4 frame{1.0f};
    core::Aabb box;
    /// World position of the middle of its visible face.
    glm::vec3 faceCenter{0.0f};
    /// World unit vectors along its face: right (as one faces it) and up, and out of it (towards the viewer).
    glm::vec3 right{1.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 out{0.0f, 0.0f, 1.0f};
};

struct SyntheticShelves {
    std::vector<world::PickTarget> targets;
    std::vector<core::Aabb> occluders;
    std::vector<SyntheticItem> items;
    world::NavRun run;
    /// World boxes of the boards alone (a subset of `occluders`), per module, bottom to top.
    std::vector<std::vector<core::Aabb>> boards;

    /// The item `id` (it must exist).
    [[nodiscard]] const SyntheticItem& item(world::PickId id) const;
    /// The item in a slot, or nullptr for an empty or dressed slot.
    [[nodiscard]] const SyntheticItem* at(int module, int level, int slot) const;
};

/// Modules side by side along the frame's +X (module m's middle at x = m * shelf::kModuleSpacing), their fronts towards
/// the frame's +Z, placed in the world by `runFrame` (a rotation about +Y and a translation). Items take ids from
/// `firstId` up, module by module, bottom to top, left to right; clusters are module * 100 + level.
[[nodiscard]] SyntheticShelves buildShelves(std::span<const ShelfModuleSpec> modules,
                                            const glm::mat4& runFrame = glm::mat4(1.0f),
                                            world::PickId firstId = 1);

/// A ray from `eye` towards `point`.
[[nodiscard]] core::Ray rayTowards(const glm::vec3& eye, const glm::vec3& point);

} // namespace rv::test
