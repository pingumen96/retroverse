// The featured wall shelf beside the hero corner (I3 step 1b): face-out items on picture ledges (ADR-0010 §3, §4).

#include <algorithm>
#include <cmath>
#include <string>

#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::formatNumber;
using detail::leaningBack;
using detail::outOfRange;
using detail::translation;

constexpr std::string_view kKind = "wall_shelf";
constexpr float kCheek = 0.02f;
constexpr float kCap = 0.025f;
constexpr float kBackBoard = 0.015f;
constexpr float kLedge = 0.018f;
constexpr float kLip = 0.008f;
constexpr float kLipHeight = 0.015f;
/// Free space between an item's front and the lip.
constexpr float kLipGap = 0.002f;
/// Free height above an item's top, under the next ledge or the cap.
constexpr float kHeadroom = 0.01f;
constexpr float kChamfer = 0.003f;
constexpr float kFitTolerance = 1e-4f;
constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;

struct WallShelfLayout {
    float innerWidth = 0.0f;
    float bottom = 0.0f;
    float top = 0.0f;
    /// Distance between the bottoms of consecutive ledges.
    float pitch = 0.0f;
    float backFace = 0.0f;
    int slotsPerLedge = 0;
    /// An item's slot origin relative to its ledge's top and the back board's face, and its extent.
    float originAbove = 0.0f;
    float originFront = 0.0f;
    float itemFront = 0.0f;  ///< the item's front-most point in front of the back board
    float itemHeight = 0.0f; ///< the item's top above the ledge's top
};

[[nodiscard]] WallShelfLayout layoutOf(const WallShelfParams& p) {
    WallShelfLayout l;
    l.innerWidth = p.width - 2.0f * kCheek;
    l.bottom = p.mountHeight;
    l.top = p.mountHeight + p.height;
    l.pitch = (p.height - kCap) / static_cast<float>(p.ledges);
    l.backFace = -0.5f * p.depth + kBackBoard;
    const float fit = (l.innerWidth - 2.0f * p.slotMargin + p.slotGap) / (p.slotSize.x + p.slotGap);
    l.slotsPerLedge = static_cast<int>(std::floor(std::max(fit, 0.0f) + kFitTolerance));
    // Leaning back by t, the item's lowest point is its back bottom edge, (d / 2) sin t below the origin, and its top
    // back edge h sin t + (d / 2) cos t behind it: there it touches the back board.
    const float tilt = core::degreesToRadians(p.tiltDeg);
    const float c = std::cos(tilt);
    const float s = std::sin(tilt);
    const glm::vec3 item = p.slotSize;
    l.originAbove = 0.5f * item.z * s;
    l.originFront = item.y * s + 0.5f * item.z * c;
    l.itemFront = item.y * s + item.z * c;
    l.itemHeight = item.z * s + item.y * c;
    return l;
}

Status validate(const WallShelfParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.3f, 3.0f),
            checkRange(kKind, "height", p.height, 0.15f, 2.0f),
            checkRange(kKind, "depth", p.depth, 0.05f, 0.4f),
            checkRange(kKind, "mountHeight", p.mountHeight, 0.0f, 2.5f),
            checkRange(kKind, "ledges", p.ledges, 1, 8),
            checkRange(kKind, "tiltDeg", p.tiltDeg, 0.0f, 30.0f),
            checkRange(kKind, "slotSize", p.slotSize, glm::vec3(0.005f), glm::vec3(0.5f)),
            checkRange(kKind, "slotGap", p.slotGap, 0.0f, 0.5f),
            checkRange(kKind, "slotMargin", p.slotMargin, 0.0f, 1.0f),
        });
        !status) {
        return status;
    }
    const WallShelfLayout l = layoutOf(p);
    if (l.slotsPerLedge < 1) {
        return outOfRange(kKind, "slotSize",
                          "no slot fits: the inner width " + formatNumber(l.innerWidth) +
                              " m minus two slot margins is narrower than the slot width " +
                              formatNumber(p.slotSize.x) + " m");
    }
    const float ledgeDepth = 0.5f * p.depth - l.backFace;
    const float needed = l.itemFront + kLipGap + kLip;
    if (needed > ledgeDepth + kFitTolerance) {
        return outOfRange(kKind, "depth",
                          "a leaning item needs " + formatNumber(needed) + " m of ledge in front of the back board, " +
                              formatNumber(ledgeDepth) + " m are left");
    }
    const float compartment = l.pitch - kLedge;
    const float neededHeight = l.itemHeight + kHeadroom;
    if (neededHeight > compartment + kFitTolerance) {
        const int fitting = static_cast<int>(std::floor((p.height - kCap) / (neededHeight + kLedge)));
        return outOfRange(kKind, fitting >= 1 ? "ledges" : "height",
                          std::to_string(p.ledges) + " ledges leave " + formatNumber(compartment) +
                              " m above each, less than the " + formatNumber(neededHeight) +
                              " m a leaning item needs (at most " + std::to_string(fitting) + " ledges fit)");
    }
    return ok();
}

ProceduralPrefab assemble(const WallShelfParams& p) {
    const WallShelfLayout l = layoutOf(p);
    detail::PrefabAssembler out(kKind, {"wood_primary", "wood_secondary"});
    MeshBuilder& wood = out.part("wood_primary");

    const float halfWidth = 0.5f * p.width;
    const float halfDepth = 0.5f * p.depth;
    const float innerHalf = 0.5f * l.innerWidth;

    // Cheeks over the whole height and depth, the cap between them (top edges bevelled).
    for (const float side : {-1.0f, 1.0f}) {
        wood.chamferedBox(glm::vec3(kCheek, p.height, p.depth), kChamfer,
                          translation(glm::vec3(side * (halfWidth - 0.5f * kCheek), l.bottom + 0.5f * p.height, 0.0f)),
                          kGrain, kWoodTile);
    }
    wood.chamferedBox(glm::vec3(l.innerWidth, kCap, p.depth), kChamfer,
                      translation(glm::vec3(0.0f, l.top - 0.5f * kCap, 0.0f)), kGrain, kWoodTile, FaceMask::PosY);
    // Back board between the cheeks, from the bottom to the cap.
    const float backHeight = p.height - kCap;
    out.part("wood_secondary")
        .box(glm::vec3(l.innerWidth, backHeight, kBackBoard),
             translation(glm::vec3(0.0f, l.bottom + 0.5f * backHeight, -halfDepth + 0.5f * kBackBoard)), kGrain,
             kWoodTile);

    // The lit interior: a strip under the cap and under every ledge but the lowest, near the front.
    if (p.boardLight) {
        constexpr float kStripThickness = 0.003f;
        constexpr float kStripDepth = 0.010f;
        constexpr float kStripBehindFront = 0.012f;
        MeshBuilder& strips = out.part("board_light");
        const glm::vec3 strip(l.innerWidth - 0.02f, kStripThickness, kStripDepth);
        const float stripZ = halfDepth - kStripBehindFront - 0.5f * kStripDepth;
        for (int ledge = 1; ledge <= p.ledges; ++ledge) {
            const float underside = ledge < p.ledges ? l.bottom + static_cast<float>(ledge) * l.pitch : l.top - kCap;
            strips.box(strip, translation(glm::vec3(0.0f, underside - 0.5f * kStripThickness, stripZ)),
                       BoxUv::PerFace, 1.0f, FaceMask::NegY | FaceMask::PosZ);
        }
    }

    // Ledges from the back board to the front, each with its lip along the front edge; the lowest is the unit's bottom.
    const float ledgeDepth = halfDepth - l.backFace;
    const glm::vec3 item = p.slotSize;
    const core::Aabb itemBounds = core::Aabb::fromMinMax(glm::vec3(-0.5f * item.x, 0.0f, -0.5f * item.z),
                                                         glm::vec3(0.5f * item.x, item.y, 0.5f * item.z));
    const float tilt = core::degreesToRadians(p.tiltDeg);
    for (int ledge = 0; ledge < p.ledges; ++ledge) {
        const float y = l.bottom + static_cast<float>(ledge) * l.pitch;
        wood.chamferedBox(glm::vec3(l.innerWidth, kLedge, ledgeDepth), kChamfer,
                          translation(glm::vec3(0.0f, y + 0.5f * kLedge, 0.5f * (l.backFace + halfDepth))), kGrain,
                          kWoodTile, FaceMask::PosY);
        wood.chamferedBox(glm::vec3(l.innerWidth, kLipHeight, kLip), kChamfer,
                          translation(glm::vec3(0.0f, y + kLedge + 0.5f * kLipHeight, halfDepth - 0.5f * kLip)), kGrain,
                          kWoodTile, FaceMask::PosY);
        // The ledge's free volume: from the back board to the lip, up to the next ledge or the cap.
        const float ledgeTop = y + kLedge;
        const float lipBack = halfDepth - kLip;
        const float free = l.pitch - kLedge;
        out.addSocket("Level_" + std::to_string(ledge), SocketKind::Anchor,
                      translation(glm::vec3(0.0f, ledgeTop, 0.5f * (l.backFace + lipBack))),
                      core::Aabb::fromMinMax(glm::vec3(-innerHalf, 0.0f, -0.5f * (lipBack - l.backFace)),
                                             glm::vec3(innerHalf, free, 0.5f * (lipBack - l.backFace))));
        const float originY = ledgeTop + l.originAbove;
        const float originZ = l.backFace + l.originFront;
        for (int index = 0; index < l.slotsPerLedge; ++index) {
            const float x =
                -innerHalf + p.slotMargin + static_cast<float>(index) * (item.x + p.slotGap) + 0.5f * item.x;
            out.addSocket("Slot_" + std::to_string(ledge) + "_" + std::to_string(index), SocketKind::ShelfSlot,
                          leaningBack(tilt, glm::vec3(x, originY, originZ)), itemBounds);
        }
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildWallShelf(const WallShelfParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
