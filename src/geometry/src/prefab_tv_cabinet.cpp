#include <algorithm>
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

constexpr std::string_view kKind = "tv_cabinet";
constexpr float kTopThickness = 0.03f;
constexpr float kTopChamfer = 0.004f;
/// The top overhangs the carcass sides by this much.
constexpr float kSideOverhang = 0.012f;
constexpr float kPanel = 0.018f;
constexpr float kPanelChamfer = 0.002f;
constexpr float kBackPanel = 0.006f;
/// Room in front of the carcass for the handles: they end flush with the front edge of the top.
constexpr float kHandleRoom = 0.02f;
constexpr float kFrontThickness = 0.018f;
constexpr float kFrontRecess = 0.003f;
constexpr float kFrontChamfer = 0.003f;
/// Gap around and between the fronts.
constexpr float kReveal = 0.003f;
constexpr float kMinDrawerHeight = 0.05f;
constexpr float kMinCarcassHeight = 0.15f;
/// Shorter legs are not built: the carcass rests on the floor instead.
constexpr float kMinLegHeight = 1e-4f;
constexpr float kLegInset = 0.045f;
constexpr float kBar = 0.01f; ///< handle bar section
/// Every wooden part: uv in meters along each board (detail::kWoodMetersPerTile).
constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;

struct CabinetLayout {
    float carcassBottom = 0.0f;
    float carcassTop = 0.0f;
    float carcassFront = 0.0f;
    float carcassBack = 0.0f;
    float sideX = 0.0f; ///< outer face of the right carcass side
    float openingBottom = 0.0f;
    float bayLeft[2] = {0.0f, 0.0f};
    float bayCenter[2] = {0.0f, 0.0f};
    float bayRight[2] = {0.0f, 0.0f};

    [[nodiscard]] float openingHeight() const noexcept { return carcassTop - openingBottom; }
};

[[nodiscard]] CabinetLayout layoutOf(const TvCabinetParams& p) {
    CabinetLayout l;
    l.carcassBottom = p.legHeight > kMinLegHeight ? p.legHeight : 0.0f;
    l.carcassTop = p.height - kTopThickness;
    l.carcassFront = 0.5f * p.depth - kHandleRoom;
    l.carcassBack = -0.5f * p.depth;
    l.sideX = 0.5f * p.width - kSideOverhang;
    l.openingBottom = l.carcassBottom + kPanel;
    // Bays between the sides and two dividers: 30% / 40% / 30% of the free width.
    const float innerLeft = -l.sideX + kPanel;
    const float innerRight = l.sideX - kPanel;
    const float free = innerRight - innerLeft - 2.0f * kPanel;
    l.bayLeft[0] = innerLeft;
    l.bayLeft[1] = innerLeft + 0.3f * free;
    l.bayCenter[0] = l.bayLeft[1] + kPanel;
    l.bayCenter[1] = l.bayCenter[0] + 0.4f * free;
    l.bayRight[0] = l.bayCenter[1] + kPanel;
    l.bayRight[1] = innerRight;
    return l;
}

Status validate(const TvCabinetParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.4f, 4.0f),
            checkRange(kKind, "height", p.height, 0.2f, 2.0f),
            checkRange(kKind, "depth", p.depth, 0.2f, 1.5f),
            checkRange(kKind, "drawers", p.drawers, 0, 8),
            checkRange(kKind, "legHeight", p.legHeight, 0.0f, 0.5f),
        });
        !status) {
        return status;
    }
    if (p.height - p.legHeight - kTopThickness < kMinCarcassHeight) {
        return outOfRange(kKind, "legHeight",
                          "legs of " + formatNumber(p.legHeight) + " m leave less than " +
                              formatNumber(kMinCarcassHeight) + " m for the carcass");
    }
    const CabinetLayout l = layoutOf(p);
    if (p.drawers > 0) {
        const float drawer =
            (l.openingHeight() - static_cast<float>(p.drawers + 1) * kReveal) / static_cast<float>(p.drawers);
        if (drawer < kMinDrawerHeight) {
            return outOfRange(kKind, "drawers",
                              std::to_string(p.drawers) + " drawers would be " + formatNumber(drawer) +
                                  " m tall (minimum " + formatNumber(kMinDrawerHeight) + " m)");
        }
    }
    return ok();
}

ProceduralPrefab assemble(const TvCabinetParams& p) {
    const CabinetLayout l = layoutOf(p);
    detail::PrefabAssembler out(kKind, {"wood_primary", "wood_secondary", "metal_trim"});
    MeshBuilder& carcass = out.part("wood_primary");
    MeshBuilder& fronts = out.part("wood_secondary");
    MeshBuilder& metal = out.part("metal_trim");

    const float carcassDepth = l.carcassFront - l.carcassBack;
    const float carcassMidZ = 0.5f * (l.carcassFront + l.carcassBack);
    const float carcassHeight = l.carcassTop - l.carcassBottom;

    // Top, overhanging the sides and reaching the front edge of the handles. Only its top edges are bevelled: a bottom
    // bevel would face away from the ceiling lights, a dark strip thinner than a pixel from afar that aliases into
    // dashes under the bright front edge.
    carcass.chamferedBox(glm::vec3(p.width, kTopThickness, p.depth), kTopChamfer,
                         translation(glm::vec3(0.0f, p.height - 0.5f * kTopThickness, 0.0f)), kGrain, kWoodTile,
                         FaceMask::PosY);
    // Sides, bottom, back and the two dividers.
    for (const float side : {-1.0f, 1.0f}) {
        carcass.chamferedBox(glm::vec3(kPanel, carcassHeight, carcassDepth), kPanelChamfer,
                             translation(glm::vec3(side * (l.sideX - 0.5f * kPanel),
                                                   l.carcassBottom + 0.5f * carcassHeight, carcassMidZ)),
                             kGrain, kWoodTile);
    }
    const float innerWidth = 2.0f * (l.sideX - kPanel);
    carcass.chamferedBox(glm::vec3(innerWidth, kPanel, carcassDepth), kPanelChamfer,
                         translation(glm::vec3(0.0f, l.carcassBottom + 0.5f * kPanel, carcassMidZ)), kGrain, kWoodTile);
    const float innerHeight = l.carcassTop - l.openingBottom;
    carcass.box(glm::vec3(innerWidth, innerHeight, kBackPanel),
                translation(glm::vec3(0.0f, l.openingBottom + 0.5f * innerHeight, l.carcassBack + 0.5f * kBackPanel)),
                kGrain, kWoodTile);
    const float dividerDepth = l.carcassFront - (l.carcassBack + kBackPanel);
    const float dividerZ = 0.5f * (l.carcassFront + l.carcassBack + kBackPanel);
    for (const float x : {l.bayLeft[1] + 0.5f * kPanel, l.bayCenter[1] + 0.5f * kPanel}) {
        carcass.chamferedBox(glm::vec3(kPanel, innerHeight, dividerDepth), kPanelChamfer,
                             translation(glm::vec3(x, l.openingBottom + 0.5f * innerHeight, dividerZ)), kGrain,
                             kWoodTile);
    }

    // Fronts sit just inside the bay openings; handles reach the front edge of the top.
    const float frontFace = l.carcassFront - kFrontRecess;
    const float frontZ = frontFace - 0.5f * kFrontThickness;
    const float barZ = 0.5f * p.depth - 0.5f * kBar;
    const float postLength = (0.5f * p.depth - kBar) - frontFace;
    const auto addFront = [&](float x0, float x1, float y0, float y1) {
        fronts.chamferedBox(glm::vec3(x1 - x0, y1 - y0, kFrontThickness), kFrontChamfer,
                            translation(glm::vec3(0.5f * (x0 + x1), 0.5f * (y0 + y1), frontZ)), kGrain, kWoodTile);
    };
    // Bar handle centered on (x, y), horizontal or vertical, on two posts.
    const auto addHandle = [&](float x, float y, float length, bool vertical) {
        const glm::vec3 bar = vertical ? glm::vec3(kBar, length, kBar) : glm::vec3(length, kBar, kBar);
        metal.box(bar, translation(glm::vec3(x, y, barZ)));
        for (const float end : {-1.0f, 1.0f}) {
            const float offset = end * 0.5f * (length - 2.0f * kBar);
            const glm::vec3 post = vertical ? glm::vec3(x, y + offset, 0.0f) : glm::vec3(x + offset, y, 0.0f);
            metal.box(glm::vec3(0.6f * kBar, 0.6f * kBar, postLength),
                      translation(glm::vec3(post.x, post.y, frontFace + 0.5f * postLength)), BoxUv::PerFace, 1.0f,
                      FaceMask::All & ~FaceMask::NegZ & ~FaceMask::PosZ);
        }
    };
    const float y0 = l.openingBottom + kReveal;
    const float y1 = l.carcassTop - kReveal;
    const float leftX0 = l.bayLeft[0] + kReveal;
    const float leftX1 = l.bayLeft[1] - kReveal;
    const float rightX0 = l.bayRight[0] + kReveal;
    const float rightX1 = l.bayRight[1] - kReveal;
    const float doorHandle = std::min(0.16f, 0.5f * (y1 - y0));
    if (p.drawers > 0) {
        const float drawer = (y1 - y0 - static_cast<float>(p.drawers - 1) * kReveal) / static_cast<float>(p.drawers);
        for (int i = 0; i < p.drawers; ++i) {
            const float top = y1 - static_cast<float>(i) * (drawer + kReveal);
            addFront(leftX0, leftX1, top - drawer, top);
            addHandle(0.5f * (leftX0 + leftX1), top - 0.5f * drawer, std::min(0.16f, 0.6f * (leftX1 - leftX0)), false);
        }
    } else {
        addFront(leftX0, leftX1, y0, y1);
        addHandle(leftX1 - 0.04f, 0.5f * (y0 + y1), doorHandle, true);
    }
    addFront(rightX0, rightX1, y0, y1);
    addHandle(rightX0 + 0.04f, 0.5f * (y0 + y1), doorHandle, true);

    // Tapered round legs under the carcass corners.
    if (l.carcassBottom > 0.0f) {
        for (const float x : {-l.sideX + kLegInset, l.sideX - kLegInset}) {
            for (const float z : {l.carcassBack + kLegInset, l.carcassFront - kLegInset}) {
                metal.frustum(0.011f, 0.016f, l.carcassBottom, 16, false, false,
                              translation(glm::vec3(x, 0.5f * l.carcassBottom, z)));
            }
        }
    }

    out.addSocket("Top", SocketKind::Anchor, translation(glm::vec3(0.0f, p.height, 0.0f)));
    const float bayWidth = l.bayCenter[1] - l.bayCenter[0];
    const float bayDepth = l.carcassFront - (l.carcassBack + kBackPanel);
    out.addSocket("ConsoleShelf", SocketKind::Anchor,
                  translation(glm::vec3(0.5f * (l.bayCenter[0] + l.bayCenter[1]), l.openingBottom, dividerZ)),
                  core::Aabb::fromMinMax(glm::vec3(-0.5f * bayWidth, 0.0f, -0.5f * bayDepth),
                                         glm::vec3(0.5f * bayWidth, innerHeight, 0.5f * bayDepth)));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildTvCabinet(const TvCabinetParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
