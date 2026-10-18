// The display case (I3 step 1b): a low case with a glass front and tilted face-out levels (ADR-0010 §3, style §8
// `furniture/display_case`).

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::formatNumber;
using detail::framePoint;
using detail::leaningBack;
using detail::outOfRange;
using detail::translation;

constexpr std::string_view kKind = "display_case";

// Base: a recessed plinth, the carcass with two doors and their knobs, the top slab (the vitrine's floor).
constexpr float kPlinth = 0.06f;
constexpr float kPlinthRecess = 0.03f;
constexpr float kBaseTop = 0.03f;
constexpr float kDoor = 0.018f;
/// The doors stand this far behind the front edge of the top slab: room for the knobs.
constexpr float kKnobRoom = 0.02f;
constexpr float kReveal = 0.003f;
constexpr float kKnobRadius = 0.009f;
constexpr float kChamfer = 0.003f;

// Vitrine: wooden sides, back and top rails, the glass front and top.
constexpr float kSide = 0.018f;
constexpr float kBack = 0.012f;
constexpr float kRailHeight = 0.03f;
constexpr float kRailDepth = 0.03f;
constexpr float kGlass = 0.005f;
/// The front glass's outer face lies this far behind the case's front.
constexpr float kGlassSet = 0.01f;
/// The top glass's upper face lies this far below the rails' top.
constexpr float kTopGlassDrop = 0.004f;

// Levels: a riser per level, its ledge (with a lip) and the panel the items lean on, tilted together.
constexpr float kLedge = 0.015f;
constexpr float kLipGap = 0.002f;
constexpr float kLip = 0.006f;
constexpr float kLipHeight = 0.012f;
constexpr float kPanel = 0.012f;
/// The panel rises this far above the items' tops.
constexpr float kPanelOver = 0.025f;
/// Level 0's riser rises this far above the vitrine's floor.
constexpr float kFirstRise = 0.02f;
/// Level 0's riser stands this far behind the front glass.
constexpr float kFrontClear = 0.03f;
constexpr float kBackClear = 0.01f;
/// Free height between the highest level and the top rails.
constexpr float kTopClear = 0.01f;
/// The tilted pieces are this much narrower than the vitrine on each side: their sides never share a plane with the
/// risers'.
constexpr float kSideInset = 0.001f;
constexpr float kFitTolerance = 1e-4f;
constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;

struct Level {
    /// The level's frame: its origin on the ledge's top at the items' mid depth, leaning back by the tilt.
    glm::mat4 frame{1.0f};
    float riserTop = 0.0f;
    float riserFront = 0.0f;
    /// The riser's back: the next level's riser front, the back panel for the last.
    float riserBack = 0.0f;
    /// Lowest z and highest y of the level's tilted pieces and items.
    float backmost = 0.0f;
    float top = 0.0f;
};

struct CaseLayout {
    float innerWidth = 0.0f;
    float floor = 0.0f;
    float interiorTop = 0.0f;
    float glassInner = 0.0f;
    float backInner = 0.0f;
    float band = 0.0f;
    int slotsPerLevel = 0;
    /// Front of the ledge and its lip in a level's frame.
    float ledgeFront = 0.0f;
    std::vector<Level> levels;
};

[[nodiscard]] CaseLayout layoutOf(const DisplayCaseParams& p) {
    CaseLayout l;
    l.innerWidth = p.width - 2.0f * kSide;
    l.floor = p.baseHeight;
    l.interiorTop = p.height - kRailHeight;
    l.glassInner = 0.5f * p.depth - kGlassSet - kGlass;
    l.backInner = -0.5f * p.depth + kBack;
    const float firstFront = l.glassInner - kFrontClear;
    l.band = (firstFront - (l.backInner + kBackClear)) / static_cast<float>(p.levels);
    const float fit = (l.innerWidth - 2.0f * p.slotMargin + p.slotGap) / (p.slotSize.x + p.slotGap);
    l.slotsPerLevel = static_cast<int>(std::floor(std::max(fit, 0.0f) + kFitTolerance));

    const glm::vec3 item = p.slotSize;
    const float tilt = core::degreesToRadians(p.tiltDeg);
    const float c = std::cos(tilt);
    const float s = std::sin(tilt);
    l.ledgeFront = 0.5f * item.z + kLipGap + kLip;
    for (int k = 0; k < p.levels; ++k) {
        Level level;
        level.riserTop = l.floor + kFirstRise + static_cast<float>(k) * p.levelRise;
        level.riserFront = firstFront - static_cast<float>(k) * l.band;
        level.riserBack = k + 1 < p.levels ? firstFront - static_cast<float>(k + 1) * l.band : l.backInner;
        // The ledge's lower front edge lies on the riser's upper front edge: (y, z) = (-kLedge, ledgeFront) in the
        // frame, rotated by the tilt.
        const glm::vec3 origin(0.0f, level.riserTop + kLedge * c - l.ledgeFront * s,
                               level.riserFront - kLedge * s - l.ledgeFront * c);
        level.frame = leaningBack(tilt, origin);
        const float panelTop = item.y + kPanelOver;
        const float panelBack = -0.5f * item.z - kPanel;
        level.backmost =
            std::min(framePoint(level.frame, panelTop, panelBack).z, framePoint(level.frame, -kLedge, panelBack).z);
        level.top = std::max({framePoint(level.frame, panelTop, -0.5f * item.z).y,
                              framePoint(level.frame, item.y, 0.5f * item.z).y,
                              framePoint(level.frame, kLipHeight, l.ledgeFront).y});
        l.levels.push_back(level);
    }
    return l;
}

Status validate(const DisplayCaseParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.4f, 4.0f),
            checkRange(kKind, "height", p.height, 0.5f, 1.6f),
            checkRange(kKind, "depth", p.depth, 0.25f, 1.2f),
            checkRange(kKind, "baseHeight", p.baseHeight, 0.15f, p.height - 0.2f),
            checkRange(kKind, "levels", p.levels, 1, 4),
            checkRange(kKind, "tiltDeg", p.tiltDeg, 0.0f, 60.0f),
            checkRange(kKind, "levelRise", p.levelRise, 0.0f, 0.5f),
            checkRange(kKind, "slotSize", p.slotSize, glm::vec3(0.005f), glm::vec3(0.5f)),
            checkRange(kKind, "slotGap", p.slotGap, 0.0f, 0.5f),
            checkRange(kKind, "slotMargin", p.slotMargin, 0.0f, 1.0f),
        });
        !status) {
        return status;
    }
    const CaseLayout l = layoutOf(p);
    if (l.slotsPerLevel < 1) {
        return outOfRange(kKind, "slotSize",
                          "no slot fits: the inner width " + formatNumber(l.innerWidth) +
                              " m minus two slot margins is narrower than the slot width " +
                              formatNumber(p.slotSize.x) + " m");
    }
    for (std::size_t k = 0; k < l.levels.size(); ++k) {
        const Level& level = l.levels[k];
        if (level.backmost < level.riserBack - kFitTolerance) {
            return outOfRange(kKind, "levels",
                              std::to_string(p.levels) + " levels leave bands of " + formatNumber(l.band) +
                                  " m in the vitrine's depth, too shallow for level " + std::to_string(k) +
                                  "'s leaning items and panel");
        }
        if (level.top > l.interiorTop - kTopClear + kFitTolerance) {
            return outOfRange(kKind, k == 0 ? "baseHeight" : "levelRise",
                              "level " + std::to_string(k) + " rises to " + formatNumber(level.top) +
                                  " m, above the vitrine's free height (up to " +
                                  formatNumber(l.interiorTop - kTopClear) + " m)");
        }
    }
    return ok();
}

ProceduralPrefab assemble(const DisplayCaseParams& p) {
    const CaseLayout l = layoutOf(p);
    detail::PrefabAssembler out(kKind, {"wood_primary", "wood_secondary", "metal_trim", "fabric", "glass"});
    MeshBuilder& wood = out.part("wood_primary");
    MeshBuilder& panels = out.part("wood_secondary");
    MeshBuilder& metal = out.part("metal_trim");
    MeshBuilder& fabric = out.part("fabric");
    MeshBuilder& glass = out.part("glass");

    const float halfWidth = 0.5f * p.width;
    const float halfDepth = 0.5f * p.depth;
    const float innerHalf = 0.5f * l.innerWidth;

    // Base: the recessed plinth, the carcass behind the doors, the top slab over everything (its top edges bevelled).
    const float carcassTop = p.baseHeight - kBaseTop;
    const float carcassFront = halfDepth - kKnobRoom - kDoor;
    wood.box(glm::vec3(p.width - 2.0f * kPlinthRecess, kPlinth, p.depth - 2.0f * kPlinthRecess),
             translation(glm::vec3(0.0f, 0.5f * kPlinth, 0.0f)), kGrain, kWoodTile, FaceMask::All & ~FaceMask::NegY);
    wood.box(glm::vec3(p.width, carcassTop - kPlinth, carcassFront + halfDepth),
             translation(glm::vec3(0.0f, 0.5f * (kPlinth + carcassTop), 0.5f * (carcassFront - halfDepth))), kGrain,
             kWoodTile);
    wood.chamferedBox(glm::vec3(p.width, kBaseTop, p.depth), kChamfer,
                      translation(glm::vec3(0.0f, carcassTop + 0.5f * kBaseTop, 0.0f)), kGrain, kWoodTile,
                      FaceMask::PosY);
    // Two doors with a knob each, near the meeting edges.
    const float doorBottom = kPlinth + kReveal;
    const float doorTop = carcassTop - kReveal;
    const float doorZ = carcassFront + 0.5f * kDoor;
    for (const float side : {-1.0f, 1.0f}) {
        const float x0 = 0.5f * kReveal;
        const float x1 = halfWidth - kReveal;
        panels.chamferedBox(glm::vec3(x1 - x0, doorTop - doorBottom, kDoor), kChamfer,
                            translation(glm::vec3(side * 0.5f * (x0 + x1), 0.5f * (doorBottom + doorTop), doorZ)),
                            kGrain, kWoodTile);
        const float knobLength = kKnobRoom - kReveal;
        glm::mat4 knob = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
        const float knobY = doorTop - std::min(0.05f, 0.5f * (doorTop - doorBottom));
        knob[3] = glm::vec4(side * (x0 + 0.05f), knobY, carcassFront + kDoor + 0.5f * knobLength, 1.0f);
        metal.cylinder(kKnobRadius, knobLength, 16, true, knob);
    }

    // Vitrine: sides over the whole depth, the back panel and the rails between them.
    const float vitrineHeight = p.height - l.floor;
    for (const float side : {-1.0f, 1.0f}) {
        wood.chamferedBox(
            glm::vec3(kSide, vitrineHeight, p.depth), kChamfer,
            translation(glm::vec3(side * (halfWidth - 0.5f * kSide), l.floor + 0.5f * vitrineHeight, 0.0f)), kGrain,
            kWoodTile, FaceMask::PosY);
    }
    panels.box(glm::vec3(l.innerWidth, l.interiorTop - l.floor, kBack),
               translation(glm::vec3(0.0f, 0.5f * (l.floor + l.interiorTop), -halfDepth + 0.5f * kBack)), kGrain,
               kWoodTile, FaceMask::All & ~FaceMask::NegY);
    for (const float side : {-1.0f, 1.0f}) {
        wood.chamferedBox(
            glm::vec3(l.innerWidth, kRailHeight, kRailDepth), kChamfer,
            translation(glm::vec3(0.0f, l.interiorTop + 0.5f * kRailHeight, side * (halfDepth - 0.5f * kRailDepth))),
            kGrain, kWoodTile, FaceMask::PosY);
    }
    // Glass: the front pane from the floor to the front rail, the top pane between the rails. Each pane is one sheet
    // in the middle of its thickness: a blended, double-sided material shows it from both sides, and a box would lay
    // two hazy layers over the items.
    glass.quad(glm::vec2(l.innerWidth, l.interiorTop - l.floor),
               translation(glm::vec3(0.0f, 0.5f * (l.floor + l.interiorTop), l.glassInner + 0.5f * kGlass)));
    glass.plane(glm::vec2(l.innerWidth, p.depth - 2.0f * kRailDepth),
                translation(glm::vec3(0.0f, p.height - kTopGlassDrop - 0.5f * kGlass, 0.0f)));

    // Levels: each riser fills its band from the floor; the tilted ledge, lip and panel stand on its front edge.
    const glm::vec3 item = p.slotSize;
    const float pieceWidth = l.innerWidth - 2.0f * kSideInset;
    const core::Aabb itemBounds = core::Aabb::fromMinMax(glm::vec3(-0.5f * item.x, 0.0f, -0.5f * item.z),
                                                         glm::vec3(0.5f * item.x, item.y, 0.5f * item.z));
    for (std::size_t k = 0; k < l.levels.size(); ++k) {
        const Level& level = l.levels[k];
        const float riserHeight = level.riserTop - l.floor;
        const float riserDepth = level.riserFront - level.riserBack;
        fabric.box(
            glm::vec3(l.innerWidth, riserHeight, riserDepth),
            translation(glm::vec3(0.0f, l.floor + 0.5f * riserHeight, 0.5f * (level.riserFront + level.riserBack))),
            BoxUv::WorldMeters, 0.25f, FaceMask::All & ~FaceMask::NegY);
        const auto tilted = [&](float y0, float y1, float z0, float z1) {
            return level.frame * translation(glm::vec3(0.0f, 0.5f * (y0 + y1), 0.5f * (z0 + z1)));
        };
        const float ledgeBack = -0.5f * item.z;
        fabric.box(glm::vec3(pieceWidth, kLedge, l.ledgeFront - ledgeBack),
                   tilted(-kLedge, 0.0f, ledgeBack, l.ledgeFront), BoxUv::WorldMeters, 0.25f);
        fabric.box(glm::vec3(pieceWidth, kLipHeight, kLip), tilted(0.0f, kLipHeight, l.ledgeFront - kLip, l.ledgeFront),
                   BoxUv::WorldMeters, 0.25f, FaceMask::All & ~FaceMask::NegY);
        const float panelTop = item.y + kPanelOver;
        fabric.box(glm::vec3(pieceWidth, panelTop + kLedge, kPanel),
                   tilted(-kLedge, panelTop, ledgeBack - kPanel, ledgeBack), BoxUv::WorldMeters, 0.25f);

        // The level's free volume: on the ledge, from the panel to the lip, up to the panel's top.
        out.addSocket("Level_" + std::to_string(k), SocketKind::Anchor, level.frame,
                      core::Aabb::fromMinMax(glm::vec3(-0.5f * pieceWidth, 0.0f, ledgeBack),
                                             glm::vec3(0.5f * pieceWidth, panelTop, l.ledgeFront - kLip)));
        for (int index = 0; index < l.slotsPerLevel; ++index) {
            const float x =
                -innerHalf + p.slotMargin + static_cast<float>(index) * (item.x + p.slotGap) + 0.5f * item.x;
            out.addSocket("Slot_" + std::to_string(k) + "_" + std::to_string(index), SocketKind::ShelfSlot,
                          translation(glm::vec3(x, 0.0f, 0.0f)) * level.frame, itemBounds);
        }
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildDisplayCase(const DisplayCaseParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
