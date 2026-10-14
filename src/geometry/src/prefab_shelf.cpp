#include <algorithm>
#include <cmath>
#include <string>

#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::formatNumber;
using detail::outOfRange;
using detail::translation;

constexpr std::string_view kKind = "shelf";
constexpr float kBackThickness = 0.008f;
/// The plinth (kick board) sits this far behind the front edge of the sides.
constexpr float kPlinthRecess = 0.012f;
constexpr float kMaxChamfer = 0.003f;
/// Absorbs float rounding when the slots fill the inner width exactly.
constexpr float kFitTolerance = 1e-4f;

struct ShelfLayout {
    float innerWidth = 0.0f;
    /// Vertical distance between consecutive boards (board thickness included).
    float pitch = 0.0f;
    float clearHeight = 0.0f;
    float boardBack = 0.0f;
    float boardFront = 0.0f;
    int slotsPerLevel = 0;

    [[nodiscard]] float boardDepth() const noexcept { return boardFront - boardBack; }
    [[nodiscard]] float boardCenterZ() const noexcept { return 0.5f * (boardFront + boardBack); }
};

[[nodiscard]] ShelfLayout layoutOf(const ShelfParams& p) {
    ShelfLayout l;
    l.innerWidth = p.width - 2.0f * p.boardThickness;
    l.pitch = (p.height - p.boardThickness - p.plinthHeight) / static_cast<float>(p.levels);
    l.clearHeight = l.pitch - p.boardThickness;
    l.boardBack = -0.5f * p.depth + (p.back ? kBackThickness : 0.0f);
    l.boardFront = 0.5f * p.depth;
    const float fit = (l.innerWidth - 2.0f * p.slotMargin + p.slotGap) / (p.slotSize.x + p.slotGap);
    l.slotsPerLevel = static_cast<int>(std::floor(std::max(fit, 0.0f) + kFitTolerance));
    return l;
}

Status validate(const ShelfParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.2f, 10.0f),
            checkRange(kKind, "height", p.height, 0.2f, 5.0f),
            checkRange(kKind, "depth", p.depth, 0.08f, 2.0f),
            checkRange(kKind, "levels", p.levels, 1, 100),
            checkRange(kKind, "boardThickness", p.boardThickness, 0.005f, 0.1f),
            checkRange(kKind, "plinthHeight", p.plinthHeight, 0.0f, 0.5f),
            checkRange(kKind, "slotSize", p.slotSize, glm::vec3(0.005f), glm::vec3(2.0f)),
            checkRange(kKind, "slotGap", p.slotGap, 0.0f, 0.5f),
            checkRange(kKind, "slotMargin", p.slotMargin, 0.0f, 1.0f),
        });
        !status) {
        return status;
    }
    const ShelfLayout l = layoutOf(p);
    if (p.slotSize.y > l.clearHeight) {
        // Blame the item when it is taller than a single compartment could be, the level count otherwise.
        const float span = p.height - p.boardThickness - p.plinthHeight;
        if (p.slotSize.y > span - p.boardThickness) {
            return outOfRange(kKind, "slotSize",
                              "slot height " + formatNumber(p.slotSize.y) + " m exceeds the free height " +
                                  formatNumber(span - p.boardThickness) + " m of a single-level shelf");
        }
        const int fitting = static_cast<int>(std::floor(span / (p.slotSize.y + p.boardThickness)));
        return outOfRange(kKind, "levels",
                          std::to_string(p.levels) + " levels leave " + formatNumber(std::max(l.clearHeight, 0.0f)) +
                              " m per compartment, less than the slot height " + formatNumber(p.slotSize.y) +
                              " m (at most " + std::to_string(fitting) + " levels fit)");
    }
    if (p.slotSize.z > l.boardDepth()) {
        return outOfRange(kKind, "slotSize",
                          "slot depth " + formatNumber(p.slotSize.z) + " m exceeds the board depth " +
                              formatNumber(l.boardDepth()) + " m");
    }
    if (l.slotsPerLevel < 1) {
        return outOfRange(kKind, "slotSize",
                          "no slot fits: the inner width " + formatNumber(l.innerWidth) +
                              " m minus two slot margins is narrower than the slot width " +
                              formatNumber(p.slotSize.x) + " m");
    }
    return ok();
}

ProceduralPrefab assemble(const ShelfParams& p) {
    const ShelfLayout l = layoutOf(p);
    detail::PrefabAssembler out(kKind, {"wood_primary", "wood_secondary"});
    MeshBuilder& wood = out.part("wood_primary");

    const float board = p.boardThickness;
    const float halfWidth = 0.5f * p.width;
    const float halfDepth = 0.5f * p.depth;
    const float chamfer = std::min(kMaxChamfer, 0.2f * board);

    // Side panels: full height and depth, they define the silhouette.
    for (const float side : {-1.0f, 1.0f}) {
        wood.chamferedBox(glm::vec3(board, p.height, p.depth), chamfer,
                          translation(glm::vec3(side * (halfWidth - 0.5f * board), 0.5f * p.height, 0.0f)));
    }
    // Top board between the sides, over the back panel.
    wood.chamferedBox(glm::vec3(l.innerWidth, board, p.depth), chamfer,
                      translation(glm::vec3(0.0f, p.height - 0.5f * board, 0.0f)));
    // Level boards, the lowest one resting on the plinth.
    for (int level = 0; level < p.levels; ++level) {
        const float bottom = p.plinthHeight + static_cast<float>(level) * l.pitch;
        wood.chamferedBox(glm::vec3(l.innerWidth, board, l.boardDepth()), chamfer,
                          translation(glm::vec3(0.0f, bottom + 0.5f * board, l.boardCenterZ())));
    }
    // Recessed kick board under the lowest board.
    if (p.plinthHeight > 1e-4f) {
        wood.box(glm::vec3(l.innerWidth, p.plinthHeight, board),
                 translation(glm::vec3(0.0f, 0.5f * p.plinthHeight, halfDepth - kPlinthRecess - 0.5f * board)),
                 BoxUv::PerFace, 1.0f, FaceMask::All & ~FaceMask::NegY);
    }
    // Back panel between the sides, from the floor to the top board.
    if (p.back) {
        const float top = p.height - board;
        out.part("wood_secondary")
            .box(glm::vec3(l.innerWidth, top, kBackThickness),
                 translation(glm::vec3(0.0f, 0.5f * top, -halfDepth + 0.5f * kBackThickness)), BoxUv::PerFace, 1.0f,
                 FaceMask::All & ~FaceMask::NegY);
    }

    // Slots: left-aligned on every board, each item centered in the board depth.
    const glm::vec3 item = p.slotSize;
    const core::Aabb itemBounds = core::Aabb::fromMinMax(glm::vec3(-0.5f * item.x, 0.0f, -0.5f * item.z),
                                                         glm::vec3(0.5f * item.x, item.y, 0.5f * item.z));
    for (int level = 0; level < p.levels; ++level) {
        const float y = p.plinthHeight + static_cast<float>(level) * l.pitch + board;
        for (int index = 0; index < l.slotsPerLevel; ++index) {
            const float x =
                -0.5f * l.innerWidth + p.slotMargin + static_cast<float>(index) * (item.x + p.slotGap) + 0.5f * item.x;
            out.addSocket("Slot_" + std::to_string(level) + "_" + std::to_string(index), SocketKind::ShelfSlot,
                          translation(glm::vec3(x, y, l.boardCenterZ())), itemBounds);
        }
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildShelf(const ShelfParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
