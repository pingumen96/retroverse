#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

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
/// Every wooden part: uv in meters along each board (detail::kWoodMetersPerTile).
constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;
/// Horizontal boards bevel the edges of their top face only (MeshBuilder::chamferedBox).
constexpr FaceMask kBoardBevels = FaceMask::PosY;

/// The slots of one kind of level: the regular levels' or the display levels'.
struct SlotRow {
    glm::vec3 item{0.0f};
    float gap = 0.0f;
    float margin = 0.0f;
    int count = 0;
};

struct ShelfLayout {
    float innerWidth = 0.0f;
    /// Vertical distance between consecutive boards (board thickness included).
    float pitch = 0.0f;
    float clearHeight = 0.0f;
    float boardBack = 0.0f;
    float boardFront = 0.0f;
    SlotRow regular;
    SlotRow display;

    [[nodiscard]] float boardDepth() const noexcept { return boardFront - boardBack; }
    [[nodiscard]] float boardCenterZ() const noexcept { return 0.5f * (boardFront + boardBack); }
};

[[nodiscard]] SlotRow slotRow(float innerWidth, const glm::vec3& item, float gap, float margin) {
    SlotRow row{item, gap, margin, 0};
    const float fit = (innerWidth - 2.0f * margin + gap) / (item.x + gap);
    row.count = static_cast<int>(std::floor(std::max(fit, 0.0f) + kFitTolerance));
    return row;
}

[[nodiscard]] ShelfLayout layoutOf(const ShelfParams& p) {
    ShelfLayout l;
    l.innerWidth = p.width - 2.0f * p.boardThickness;
    l.pitch = (p.height - p.boardThickness - p.plinthHeight) / static_cast<float>(p.levels);
    l.clearHeight = l.pitch - p.boardThickness;
    l.boardBack = -0.5f * p.depth + (p.back ? kBackThickness : 0.0f);
    l.boardFront = 0.5f * p.depth;
    l.regular = slotRow(l.innerWidth, p.slotSize, p.slotGap, p.slotMargin);
    l.display = slotRow(l.innerWidth, p.displaySlotSize, p.displaySlotGap, p.displaySlotMargin);
    return l;
}

[[nodiscard]] bool isDisplayLevel(const ShelfParams& p, int level) {
    return std::find(p.displayLevels.begin(), p.displayLevels.end(), level) != p.displayLevels.end();
}

/// The checks the items of one kind of level must pass besides their height: they fit the board's depth (with the
/// front inset), and at least one fits across. `sizeKey` is "slotSize" or "displaySlotSize".
Status validateRow(const ShelfParams& p, const ShelfLayout& l, const SlotRow& row, std::string_view sizeKey) {
    if (row.item.z > l.boardDepth()) {
        return outOfRange(kKind, sizeKey,
                          "slot depth " + formatNumber(row.item.z) + " m exceeds the board depth " +
                              formatNumber(l.boardDepth()) + " m");
    }
    if (p.slotFrontInset && *p.slotFrontInset + row.item.z > l.boardDepth() + kFitTolerance) {
        return outOfRange(kKind, "slotFrontInset",
                          "an item " + formatNumber(row.item.z) + " m deep set " + formatNumber(*p.slotFrontInset) +
                              " m behind the front edge overhangs the back of the " + formatNumber(l.boardDepth()) +
                              " m boards");
    }
    if (row.count < 1) {
        return outOfRange(kKind, sizeKey,
                          "no slot fits: the inner width " + formatNumber(l.innerWidth) +
                              " m minus two slot margins is narrower than the slot width " + formatNumber(row.item.x) +
                              " m");
    }
    return ok();
}

/// An item taller than the compartments: blamed on the item when it is taller than a single compartment could be, on
/// the level count otherwise.
Status validateRowHeight(const ShelfParams& p, const ShelfLayout& l, const SlotRow& row, std::string_view sizeKey) {
    if (row.item.y <= l.clearHeight) {
        return ok();
    }
    const float span = p.height - p.boardThickness - p.plinthHeight;
    const float singleLevel = span - p.boardThickness;
    if (row.item.y > singleLevel) {
        return outOfRange(kKind, sizeKey,
                          "slot height " + formatNumber(row.item.y) + " m exceeds the free height " +
                              formatNumber(singleLevel) + " m of a single-level shelf");
    }
    const int fitting = static_cast<int>(std::floor(span / (row.item.y + p.boardThickness)));
    return outOfRange(kKind, "levels",
                      std::to_string(p.levels) + " levels leave " + formatNumber(std::max(l.clearHeight, 0.0f)) +
                          " m per compartment, less than the slot height " + formatNumber(row.item.y) + " m (at most " +
                          std::to_string(fitting) + " levels fit)");
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
            p.slotFrontInset ? checkRange(kKind, "slotFrontInset", *p.slotFrontInset, 0.0f, 2.0f) : ok(),
            checkRange(kKind, "displaySlotSize", p.displaySlotSize, glm::vec3(0.005f), glm::vec3(2.0f)),
            checkRange(kKind, "displaySlotGap", p.displaySlotGap, 0.0f, 0.5f),
            checkRange(kKind, "displaySlotMargin", p.displaySlotMargin, 0.0f, 1.0f),
        });
        !status) {
        return status;
    }
    for (auto it = p.displayLevels.begin(); it != p.displayLevels.end(); ++it) {
        if (*it < 0 || *it >= p.levels) {
            return outOfRange(kKind, "displayLevels",
                              "level " + std::to_string(*it) + " is outside [0, " + std::to_string(p.levels - 1) + "]");
        }
        if (std::find(p.displayLevels.begin(), it, *it) != it) {
            return outOfRange(kKind, "displayLevels", "level " + std::to_string(*it) + " is listed twice");
        }
    }
    const ShelfLayout l = layoutOf(p);
    // The free height of a single-level shelf: the plinth and two boards must leave some.
    const float singleLevel = p.height - 2.0f * p.boardThickness - p.plinthHeight;
    if (!(singleLevel > 0.0f)) {
        // No compartment at all, whatever the item: blame the plinth, or the boards when there is none.
        return outOfRange(kKind, p.plinthHeight > 0.0f ? "plinthHeight" : "boardThickness",
                          "a " + formatNumber(p.plinthHeight) + " m plinth and two " + formatNumber(p.boardThickness) +
                              " m boards leave no free height in a " + formatNumber(p.height) + " m tall shelf");
    }
    // A display level for every level leaves the regular slots unused, and no display level the display slots.
    const bool regularUsed = static_cast<int>(p.displayLevels.size()) < p.levels;
    const bool displayUsed = !p.displayLevels.empty();
    if (Status status = firstError({
            regularUsed ? validateRowHeight(p, l, l.regular, "slotSize") : ok(),
            displayUsed ? validateRowHeight(p, l, l.display, "displaySlotSize") : ok(),
            regularUsed ? validateRow(p, l, l.regular, "slotSize") : ok(),
            displayUsed ? validateRow(p, l, l.display, "displaySlotSize") : ok(),
        });
        !status) {
        return status;
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
                          translation(glm::vec3(side * (halfWidth - 0.5f * board), 0.5f * p.height, 0.0f)), kGrain,
                          kWoodTile);
    }
    // Top board between the sides, over the back panel, and the level boards, the lowest one resting on the plinth.
    // Horizontal boards bevel their top edges only: the front one catches the ceiling lights, while a bottom bevel
    // would face away from them, a dark strip thinner than a pixel from a few meters away that aliases into dashes.
    wood.chamferedBox(glm::vec3(l.innerWidth, board, p.depth), chamfer,
                      translation(glm::vec3(0.0f, p.height - 0.5f * board, 0.0f)), kGrain, kWoodTile, kBoardBevels);
    for (int level = 0; level < p.levels; ++level) {
        const float bottom = p.plinthHeight + static_cast<float>(level) * l.pitch;
        wood.chamferedBox(glm::vec3(l.innerWidth, board, l.boardDepth()), chamfer,
                          translation(glm::vec3(0.0f, bottom + 0.5f * board, l.boardCenterZ())), kGrain, kWoodTile,
                          kBoardBevels);
    }
    // Recessed kick board under the lowest board.
    if (p.plinthHeight > 1e-4f) {
        wood.box(glm::vec3(l.innerWidth, p.plinthHeight, board),
                 translation(glm::vec3(0.0f, 0.5f * p.plinthHeight, halfDepth - kPlinthRecess - 0.5f * board)), kGrain,
                 kWoodTile, FaceMask::All & ~FaceMask::NegY);
    }
    // Back panel between the sides, from the floor to the top board.
    if (p.back) {
        const float top = p.height - board;
        out.part("wood_secondary")
            .box(glm::vec3(l.innerWidth, top, kBackThickness),
                 translation(glm::vec3(0.0f, 0.5f * top, -halfDepth + 0.5f * kBackThickness)), kGrain, kWoodTile,
                 FaceMask::All & ~FaceMask::NegY);
    }

    // Per level its Level socket (the compartment's free volume on the board), then its slots: left-aligned, each item
    // centered in the board depth or set slotFrontInset behind its front edge; the display levels with their own item,
    // gap and margin.
    const core::Aabb compartment =
        core::Aabb::fromMinMax(glm::vec3(-0.5f * l.innerWidth, 0.0f, -0.5f * l.boardDepth()),
                               glm::vec3(0.5f * l.innerWidth, l.clearHeight, 0.5f * l.boardDepth()));
    for (int level = 0; level < p.levels; ++level) {
        const SlotRow& row = isDisplayLevel(p, level) ? l.display : l.regular;
        const glm::vec3 item = row.item;
        const core::Aabb itemBounds = core::Aabb::fromMinMax(glm::vec3(-0.5f * item.x, 0.0f, -0.5f * item.z),
                                                             glm::vec3(0.5f * item.x, item.y, 0.5f * item.z));
        const float z = p.slotFrontInset ? l.boardFront - *p.slotFrontInset - 0.5f * item.z : l.boardCenterZ();
        const float y = p.plinthHeight + static_cast<float>(level) * l.pitch + board;
        out.addSocket("Level_" + std::to_string(level), SocketKind::Anchor,
                      translation(glm::vec3(0.0f, y, l.boardCenterZ())), compartment);
        for (int index = 0; index < row.count; ++index) {
            const float x =
                -0.5f * l.innerWidth + row.margin + static_cast<float>(index) * (item.x + row.gap) + 0.5f * item.x;
            out.addSocket("Slot_" + std::to_string(level) + "_" + std::to_string(index), SocketKind::ShelfSlot,
                          translation(glm::vec3(x, y, z)), itemBounds);
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
