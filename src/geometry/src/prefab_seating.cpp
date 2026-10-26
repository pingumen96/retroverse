// I6 step 3b's era seating (ADR-0014 §1 amended: furniture a theme pack chooses within the footprint of what it
// replaces): the sofa, the bean bag, the office chair and the bar stool. Original designs, built like the armchair.

#include <algorithm>
#include <array>
#include <cmath>

#include <glm/ext/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::formatNumber;
using detail::outOfRange;
using detail::translation;

/// Seat cushion thickness, back thickness and back cushion thickness (meters), as the armchair's.
constexpr float kSeatCushion = 0.12f;
constexpr float kBackThickness = 0.16f;
constexpr float kBackCushion = 0.11f;
constexpr float kChamfer = 0.025f;
constexpr float kBackTiltDeg = 10.0f;
constexpr float kBackCushionClearance = 0.03f;
constexpr float kLegInset = 0.06f;
constexpr int kLegSegments = 16;
/// Shorter legs are not built: the piece rests on the floor.
constexpr float kMinLegHeight = 1e-4f;
/// Smallest step between the base's bottom and the seat, the seat and the arms, the arms and the back.
constexpr float kMinStep = 0.05f;
/// Divisible by 4: round parts reach their radius exactly on the X and Z axes (exact bounds).
constexpr int kRoundSegments = 32;

glm::mat4 scaled(const glm::vec3& position, const glm::vec3& scale) {
    return glm::scale(translation(position), scale);
}

/// A rigid frame at `position` turned by `yaw` about +Y, then leaning back by `tilt` about its +X.
glm::mat4 turnedAndTilted(const glm::vec3& position, float yaw, float tilt) {
    glm::mat4 frame = glm::rotate(translation(position), yaw, glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::rotate(frame, -tilt, glm::vec3(1.0f, 0.0f, 0.0f));
}

// --- sofa -----------------------------------------------------------------------------------------------------

constexpr std::string_view kSofa = "sofa";
constexpr float kCushionGap = 0.004f;
/// A throw pillow's side and thickness, and how far it leans back.
constexpr float kPillowSide = 0.36f;
constexpr float kPillowThickness = 0.11f;
constexpr float kPillowTiltDeg = 18.0f;
constexpr float kPillowTurnDeg = 12.0f;

float sofaArmWidth(const SofaParams& p) noexcept {
    return std::clamp(0.1f * p.width, 0.12f, 0.2f);
}

Status validateSofa(const SofaParams& p) {
    if (Status status = firstError({
            checkRange(kSofa, "width", p.width, 1.0f, 3.0f),
            checkRange(kSofa, "depth", p.depth, 0.6f, 1.2f),
            checkRange(kSofa, "height", p.height, 0.5f, 1.2f),
            checkRange(kSofa, "seatHeight", p.seatHeight, 0.25f, 0.6f),
            checkRange(kSofa, "armHeight", p.armHeight, 0.3f, 0.9f),
            checkRange(kSofa, "legHeight", p.legHeight, 0.0f, 0.3f),
            checkRange(kSofa, "seats", p.seats, 1, 4),
        });
        !status) {
        return status;
    }
    if (p.seatHeight - kSeatCushion < p.legHeight + kMinStep) {
        return outOfRange(kSofa, "legHeight",
                          "legs of " + formatNumber(p.legHeight) + " m leave less than " + formatNumber(kMinStep) +
                              " m of base under a seat at " + formatNumber(p.seatHeight) + " m");
    }
    if (p.armHeight < p.seatHeight + kMinStep) {
        return outOfRange(kSofa, "armHeight",
                          "the armrests must rise at least " + formatNumber(kMinStep) + " m above the seat at " +
                              formatNumber(p.seatHeight) + " m");
    }
    if (p.height < p.armHeight + kMinStep) {
        return outOfRange(kSofa, "height",
                          "the back must rise at least " + formatNumber(kMinStep) + " m above the armrests at " +
                              formatNumber(p.armHeight) + " m");
    }
    return ok();
}

ProceduralPrefab assembleSofa(const SofaParams& p) {
    detail::PrefabAssembler out(kSofa, {"fabric", "wood", "pillow"});
    MeshBuilder& fabric = out.part("fabric");

    const float halfWidth = 0.5f * p.width;
    const float halfDepth = 0.5f * p.depth;
    const float bottom = p.legHeight > kMinLegHeight ? p.legHeight : 0.0f;
    const float armWidth = sofaArmWidth(p);
    const float innerWidth = p.width - 2.0f * armWidth;
    const float seatBase = p.seatHeight - kSeatCushion;
    const float backFront = -halfDepth + kBackThickness;

    // The armchair's back, armrests and base, as wide as the sofa.
    fabric.chamferedBox(glm::vec3(p.width, p.height - bottom, kBackThickness), kChamfer,
                        translation(glm::vec3(0.0f, 0.5f * (bottom + p.height), -halfDepth + 0.5f * kBackThickness)));
    for (const float side : {-1.0f, 1.0f}) {
        fabric.chamferedBox(glm::vec3(armWidth, p.armHeight - bottom, halfDepth - backFront), kChamfer,
                            translation(glm::vec3(side * (halfWidth - 0.5f * armWidth), 0.5f * (bottom + p.armHeight),
                                                  0.5f * (backFront + halfDepth))));
    }
    fabric.chamferedBox(glm::vec3(innerWidth, seatBase - bottom, halfDepth - backFront), kChamfer,
                        translation(glm::vec3(0.0f, 0.5f * (bottom + seatBase), 0.5f * (backFront + halfDepth))));

    // `seats` seat cushions and back cushions side by side.
    const int seats = p.seats;
    const float usable = innerWidth - 0.01f - static_cast<float>(seats - 1) * kCushionGap;
    const float cushionWidth = usable / static_cast<float>(seats);
    const float seatFront = halfDepth - 0.005f;
    const float tilt = core::degreesToRadians(kBackTiltDeg);
    const glm::vec3 up(0.0f, std::cos(tilt), -std::sin(tilt));
    const glm::vec3 front(0.0f, std::sin(tilt), std::cos(tilt));
    const float cushionHeight = (p.height - kBackCushionClearance - p.seatHeight) / std::cos(tilt);
    for (int i = 0; i < seats; ++i) {
        const float x = -0.5f * innerWidth + 0.005f + 0.5f * cushionWidth +
                        static_cast<float>(i) * (cushionWidth + kCushionGap);
        fabric.chamferedBox(glm::vec3(cushionWidth, kSeatCushion, seatFront - backFront), kChamfer,
                            translation(glm::vec3(x, seatBase + 0.5f * kSeatCushion, 0.5f * (backFront + seatFront))));
        const glm::vec3 lowerFrontEdge(x, p.seatHeight,
                                       backFront + cushionHeight * std::sin(tilt) + kBackCushion * std::cos(tilt));
        const glm::vec3 center = lowerFrontEdge + up * (0.5f * cushionHeight) - front * (0.5f * kBackCushion);
        glm::mat4 cushionFrame = glm::rotate(glm::mat4(1.0f), -tilt, glm::vec3(1.0f, 0.0f, 0.0f));
        cushionFrame[3] = glm::vec4(center, 1.0f);
        fabric.chamferedBox(glm::vec3(cushionWidth, cushionHeight, kBackCushion), kChamfer, cushionFrame);
    }

    // A throw pillow leaning in each corner, against the back cushions, turned a little towards the middle.
    const float pillow = std::min(kPillowSide, p.height - p.seatHeight - 0.04f);
    if (p.pillows && pillow >= 0.15f && innerWidth >= 2.0f * pillow) {
        MeshBuilder& pillows = out.part("pillow");
        const float pillowTilt = core::degreesToRadians(kPillowTiltDeg);
        const float pillowZ = backFront + kBackCushion + 0.5f * kPillowThickness + 0.5f * pillow * std::sin(pillowTilt);
        for (const float side : {-1.0f, 1.0f}) {
            const float x = side * (0.5f * innerWidth - 0.5f * pillow - 0.02f);
            const glm::vec3 centre(x, p.seatHeight + 0.5f * pillow * std::cos(pillowTilt) + 0.005f, pillowZ);
            pillows.chamferedBox(glm::vec3(pillow, pillow, kPillowThickness), 0.03f,
                                 turnedAndTilted(centre, -side * core::degreesToRadians(kPillowTurnDeg), pillowTilt));
        }
    }

    // Tapered round legs under the corners, and two in the middle under a long sofa.
    if (bottom > 0.0f) {
        MeshBuilder& wood = out.part("wood");
        std::array<float, 3> xs{-halfWidth + kLegInset, halfWidth - kLegInset, 0.0f};
        const std::size_t count = p.width > 1.8f ? 3 : 2;
        for (std::size_t i = 0; i < count; ++i) {
            for (const float z : {-halfDepth + kLegInset, halfDepth - kLegInset}) {
                wood.frustum(0.016f, 0.024f, bottom, kLegSegments, true, false,
                             translation(glm::vec3(xs[i], 0.5f * bottom, z)));
            }
        }
    }

    out.addSocket("Seat", SocketKind::Anchor, translation(glm::vec3(0.0f, p.seatHeight, 0.5f * (backFront + seatFront))));
    return std::move(out).finish();
}

// --- beanbag --------------------------------------------------------------------------------------------------

constexpr std::string_view kBeanbag = "beanbag";
/// The body's height and the back lump's, as shares of the bean bag's height; the lump's width and depth as shares.
constexpr float kBodyHeight = 0.62f;
constexpr float kLumpHeight = 0.78f;
constexpr float kLumpWidth = 0.92f;
constexpr float kLumpDepth = 0.46f;

Status validateBeanbag(const BeanbagParams& p) {
    return firstError({
        checkRange(kBeanbag, "width", p.width, 0.4f, 1.5f),
        checkRange(kBeanbag, "depth", p.depth, 0.4f, 1.5f),
        checkRange(kBeanbag, "height", p.height, 0.3f, 1.2f),
    });
}

ProceduralPrefab assembleBeanbag(const BeanbagParams& p) {
    detail::PrefabAssembler out(kBeanbag, {"fabric"});
    MeshBuilder& fabric = out.part("fabric");
    // A squat body on the floor (a unit sphere scaled: its equator reaches the width and the depth)...
    fabric.uvSphere(0.5f, kRoundSegments, 16,
                    scaled(glm::vec3(0.0f, 0.5f * kBodyHeight * p.height, 0.0f),
                           glm::vec3(p.width, kBodyHeight * p.height, p.depth)));
    // ...and the lump of its back, slumped against it, reaching the top.
    const float lumpHeight = kLumpHeight * p.height;
    fabric.uvSphere(0.5f, kRoundSegments, 16,
                    scaled(glm::vec3(0.0f, p.height - 0.5f * lumpHeight, -0.5f * p.depth + 0.5f * kLumpDepth * p.depth +
                                                                              0.04f * p.depth),
                           glm::vec3(kLumpWidth * p.width, lumpHeight, kLumpDepth * p.depth)));
    return std::move(out).finish();
}

// --- office chair ---------------------------------------------------------------------------------------------

constexpr std::string_view kOfficeChair = "office_chair";
constexpr float kChairSeatThickness = 0.08f;
constexpr float kChairBackThickness = 0.07f;
constexpr float kChairArmWidth = 0.05f;
constexpr float kCasterRadius = 0.025f;
constexpr int kStarArms = 5;
/// The back's lower edge above the seat.
constexpr float kBackGap = 0.12f;

Status validateOfficeChair(const OfficeChairParams& p) {
    if (Status status = firstError({
            checkRange(kOfficeChair, "width", p.width, 0.45f, 0.8f),
            checkRange(kOfficeChair, "depth", p.depth, 0.45f, 0.8f),
            checkRange(kOfficeChair, "height", p.height, 0.7f, 1.3f),
            checkRange(kOfficeChair, "seatHeight", p.seatHeight, 0.35f, 0.6f),
        });
        !status) {
        return status;
    }
    if (p.height < p.seatHeight + 0.25f) {
        return outOfRange(kOfficeChair, "height",
                          "the back must rise at least 0.25 m above the seat at " + formatNumber(p.seatHeight) + " m");
    }
    return ok();
}

ProceduralPrefab assembleOfficeChair(const OfficeChairParams& p) {
    detail::PrefabAssembler out(kOfficeChair, {"fabric", "plastic_dark", "metal"});
    MeshBuilder& fabric = out.part("fabric");
    MeshBuilder& dark = out.part("plastic_dark");
    MeshBuilder& metal = out.part("metal");

    const float halfDepth = 0.5f * p.depth;
    const float seatWidth = p.arms ? p.width - 2.0f * kChairArmWidth - 0.02f : p.width;
    const float seatBottom = p.seatHeight - kChairSeatThickness;
    // The seat, from the back's front to the chair's front.
    const float seatBack = -halfDepth + kChairBackThickness + 0.03f;
    fabric.chamferedBox(glm::vec3(seatWidth, kChairSeatThickness, halfDepth - seatBack), 0.025f,
                        translation(glm::vec3(0.0f, seatBottom + 0.5f * kChairSeatThickness,
                                              0.5f * (seatBack + halfDepth))));
    // The back on its stem, its rear face at the chair's back.
    const float backBottom = p.seatHeight + kBackGap;
    fabric.chamferedBox(glm::vec3(0.92f * seatWidth, p.height - backBottom, kChairBackThickness), 0.025f,
                        translation(glm::vec3(0.0f, 0.5f * (backBottom + p.height),
                                              -halfDepth + 0.5f * kChairBackThickness)));
    const float stemZ = -halfDepth + 0.5f * kChairBackThickness;
    dark.box(glm::vec3(0.06f, backBottom - seatBottom + 0.02f, 0.025f),
             translation(glm::vec3(0.0f, 0.5f * (seatBottom + backBottom), stemZ)));
    dark.box(glm::vec3(0.06f, 0.02f, seatBack - stemZ + 0.04f),
             translation(glm::vec3(0.0f, seatBottom - 0.01f, 0.5f * (stemZ + seatBack + 0.04f))));

    // Armrests: a post from under the seat and a pad.
    if (p.arms) {
        const float padTop = p.seatHeight + 0.2f;
        for (const float side : {-1.0f, 1.0f}) {
            const float x = side * (0.5f * p.width - 0.5f * kChairArmWidth);
            dark.box(glm::vec3(0.025f, padTop - seatBottom - 0.03f, 0.04f),
                     translation(glm::vec3(x, 0.5f * (seatBottom + padTop - 0.03f), -0.02f)));
            dark.chamferedBox(glm::vec3(kChairArmWidth, 0.03f, 0.24f), 0.008f,
                              translation(glm::vec3(x, padTop - 0.015f, 0.0f)));
            dark.box(glm::vec3(0.5f * p.width - 0.5f * seatWidth, 0.02f, 0.04f),
                     translation(glm::vec3(side * 0.25f * (p.width + seatWidth), seatBottom - 0.01f, -0.02f)));
        }
    }

    // The gas lift on a hub, the five-star base, a caster under each arm's end.
    const float reach = 0.5f * std::min(p.width, p.depth) - kCasterRadius;
    const float hubTop = 0.1f;
    metal.cylinder(0.022f, seatBottom - 0.02f - hubTop, 24, true,
                   translation(glm::vec3(0.0f, 0.5f * (hubTop + seatBottom - 0.02f), 0.0f)));
    dark.cylinder(0.035f, 0.05f, 24, true, translation(glm::vec3(0.0f, hubTop - 0.025f, 0.0f)));
    dark.frustum(0.045f, 0.03f, 0.04f, 24, true, false,
                 translation(glm::vec3(0.0f, seatBottom - 0.04f, 0.0f)));
    for (int i = 0; i < kStarArms; ++i) {
        const float angle = core::kTwoPi * static_cast<float>(i) / static_cast<float>(kStarArms);
        const glm::vec3 direction(std::sin(angle), 0.0f, std::cos(angle));
        const float length = reach - 0.02f;
        glm::mat4 arm = glm::rotate(glm::mat4(1.0f), angle, glm::vec3(0.0f, 1.0f, 0.0f));
        arm[3] = glm::vec4(direction * (0.5f * length + 0.02f) + glm::vec3(0.0f, 0.065f, 0.0f), 1.0f);
        dark.chamferedBox(glm::vec3(0.035f, 0.03f, length), 0.006f, arm);
        dark.uvSphere(kCasterRadius, 16, 8, translation(direction * reach + glm::vec3(0.0f, kCasterRadius, 0.0f)));
    }

    out.addSocket("Seat", SocketKind::Anchor, translation(glm::vec3(0.0f, p.seatHeight, 0.5f * (seatBack + halfDepth))));
    return std::move(out).finish();
}

// --- bar stool ------------------------------------------------------------------------------------------------

constexpr std::string_view kBarStool = "bar_stool";
constexpr float kStoolCushion = 0.05f;
constexpr float kStoolPan = 0.03f;
constexpr float kStoolBase = 0.025f;
constexpr float kStoolPole = 0.024f;

Status validateBarStool(const BarStoolParams& p) {
    return firstError({
        checkRange(kBarStool, "height", p.height, 0.45f, 1.0f),
        checkRange(kBarStool, "seatRadius", p.seatRadius, 0.12f, 0.3f),
        checkRange(kBarStool, "baseRadius", p.baseRadius, 0.12f, 0.35f),
    });
}

ProceduralPrefab assembleBarStool(const BarStoolParams& p) {
    detail::PrefabAssembler out(kBarStool, {"fabric", "metal"});
    MeshBuilder& metal = out.part("metal");
    const float cushionBottom = p.height - kStoolCushion;
    const float panBottom = cushionBottom - kStoolPan;
    // A weighted disc base, the pole, a foot ring on four spokes, the seat's pan.
    metal.frustum(p.baseRadius, 0.85f * p.baseRadius, kStoolBase, kRoundSegments, true, false,
                  translation(glm::vec3(0.0f, 0.5f * kStoolBase, 0.0f)));
    metal.cylinder(kStoolPole, panBottom - kStoolBase, 24, false,
                   translation(glm::vec3(0.0f, 0.5f * (kStoolBase + panBottom), 0.0f)));
    const float ringY = 0.4f * p.height;
    const float ringRadius = std::min(0.8f * p.baseRadius, p.seatRadius) ;
    metal.frustum(ringRadius, ringRadius, 0.016f, kRoundSegments, false, true,
                  translation(glm::vec3(0.0f, ringY, 0.0f)));
    for (int i = 0; i < 4; ++i) {
        glm::mat4 spoke = glm::rotate(glm::mat4(1.0f), core::kHalfPi * static_cast<float>(i), glm::vec3(0, 1, 0));
        spoke[3] = glm::vec4(glm::vec3(spoke[2]) * (0.5f * ringRadius) + glm::vec3(0.0f, ringY, 0.0f), 1.0f);
        metal.box(glm::vec3(0.012f, 0.012f, ringRadius), spoke);
    }
    metal.frustum(0.8f * p.seatRadius, 0.95f * p.seatRadius, kStoolPan, kRoundSegments, true, false,
                  translation(glm::vec3(0.0f, panBottom + 0.5f * kStoolPan, 0.0f)));
    // The padded seat, its top edge softened.
    out.part("fabric").frustum(p.seatRadius, 0.92f * p.seatRadius, kStoolCushion, kRoundSegments, true, false,
                               translation(glm::vec3(0.0f, cushionBottom + 0.5f * kStoolCushion, 0.0f)));
    out.addSocket("Seat", SocketKind::Anchor, translation(glm::vec3(0.0f, p.height, 0.0f)));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildSofa(const SofaParams& params) {
    if (Status status = validateSofa(params); !status) {
        return propagate(status.error());
    }
    return assembleSofa(params);
}

Expected<ProceduralPrefab> buildBeanbag(const BeanbagParams& params) {
    if (Status status = validateBeanbag(params); !status) {
        return propagate(status.error());
    }
    return assembleBeanbag(params);
}

Expected<ProceduralPrefab> buildOfficeChair(const OfficeChairParams& params) {
    if (Status status = validateOfficeChair(params); !status) {
        return propagate(status.error());
    }
    return assembleOfficeChair(params);
}

Expected<ProceduralPrefab> buildBarStool(const BarStoolParams& params) {
    if (Status status = validateBarStool(params); !status) {
        return propagate(status.error());
    }
    return assembleBarStool(params);
}

} // namespace rv::geometry
