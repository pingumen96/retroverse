#include <algorithm>
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

constexpr std::string_view kKind = "armchair";
/// Seat cushion thickness, back thickness and back cushion thickness (meters).
constexpr float kSeatCushion = 0.12f;
constexpr float kBackThickness = 0.16f;
constexpr float kBackCushion = 0.11f;
/// Armrest width as a share of the chair's width.
constexpr float kArmShare = 0.17f;
/// Soft upholstered edges.
constexpr float kChamfer = 0.025f;
/// The back cushion leans back by this much against the back.
constexpr float kBackTiltDeg = 10.0f;
/// Room left above the back cushion under the top of the back.
constexpr float kBackCushionClearance = 0.03f;
constexpr float kLegInset = 0.06f;
constexpr int kLegSegments = 16;
/// Shorter legs are not built: the chair rests on the floor.
constexpr float kMinLegHeight = 1e-4f;
/// Smallest height of the base under the seat cushion, and of the steps between seat, arms and back.
constexpr float kMinStep = 0.05f;

Status validate(const ArmchairParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.5f, 2.5f),
            checkRange(kKind, "depth", p.depth, 0.5f, 1.5f),
            checkRange(kKind, "height", p.height, 0.5f, 1.4f),
            checkRange(kKind, "seatHeight", p.seatHeight, 0.25f, 0.7f),
            checkRange(kKind, "armHeight", p.armHeight, 0.3f, 1.0f),
            checkRange(kKind, "legHeight", p.legHeight, 0.0f, 0.3f),
        });
        !status) {
        return status;
    }
    if (p.seatHeight - kSeatCushion < p.legHeight + kMinStep) {
        return outOfRange(kKind, "legHeight",
                          "legs of " + formatNumber(p.legHeight) + " m leave less than " + formatNumber(kMinStep) +
                              " m of base under a seat at " + formatNumber(p.seatHeight) + " m");
    }
    if (p.armHeight < p.seatHeight + kMinStep) {
        return outOfRange(kKind, "armHeight",
                          "the armrests must rise at least " + formatNumber(kMinStep) + " m above the seat at " +
                              formatNumber(p.seatHeight) + " m");
    }
    if (p.height < p.armHeight + kMinStep) {
        return outOfRange(kKind, "height",
                          "the back must rise at least " + formatNumber(kMinStep) + " m above the armrests at " +
                              formatNumber(p.armHeight) + " m");
    }
    return ok();
}

ProceduralPrefab assemble(const ArmchairParams& p) {
    detail::PrefabAssembler out(kKind, {"fabric", "wood"});
    MeshBuilder& fabric = out.part("fabric");

    const float halfWidth = 0.5f * p.width;
    const float halfDepth = 0.5f * p.depth;
    const float bottom = p.legHeight > kMinLegHeight ? p.legHeight : 0.0f;
    const float armWidth = kArmShare * p.width;
    const float innerWidth = p.width - 2.0f * armWidth;
    const float seatBase = p.seatHeight - kSeatCushion;
    const float backFront = -halfDepth + kBackThickness;

    // Back over the full width, armrests from the back to the front, and the base between them under the seat (the
    // pieces meet face to face: no coplanar surfaces facing the same way).
    fabric.chamferedBox(glm::vec3(p.width, p.height - bottom, kBackThickness), kChamfer,
                        translation(glm::vec3(0.0f, 0.5f * (bottom + p.height), -halfDepth + 0.5f * kBackThickness)));
    for (const float side : {-1.0f, 1.0f}) {
        fabric.chamferedBox(glm::vec3(armWidth, p.armHeight - bottom, halfDepth - backFront), kChamfer,
                            translation(glm::vec3(side * (halfWidth - 0.5f * armWidth), 0.5f * (bottom + p.armHeight),
                                                  0.5f * (backFront + halfDepth))));
    }
    fabric.chamferedBox(glm::vec3(innerWidth, seatBase - bottom, halfDepth - backFront), kChamfer,
                        translation(glm::vec3(0.0f, 0.5f * (bottom + seatBase), 0.5f * (backFront + halfDepth))));

    // Seat cushion between the arms, a little proud of the base at the front.
    const float seatFront = halfDepth - 0.005f;
    fabric.chamferedBox(glm::vec3(innerWidth - 0.01f, kSeatCushion, seatFront - backFront), kChamfer,
                        translation(glm::vec3(0.0f, seatBase + 0.5f * kSeatCushion, 0.5f * (backFront + seatFront))));

    // Back cushion leaning back: its lower front edge on the seat, its upper back edge against the back. Its frame: up
    // tilted back by kBackTiltDeg, front following.
    const float tilt = core::degreesToRadians(kBackTiltDeg);
    const glm::vec3 up(0.0f, std::cos(tilt), -std::sin(tilt));
    const glm::vec3 front(0.0f, std::sin(tilt), std::cos(tilt));
    const float cushionHeight = (p.height - kBackCushionClearance - p.seatHeight) / std::cos(tilt);
    const glm::vec3 lowerFrontEdge(0.0f, p.seatHeight,
                                   backFront + cushionHeight * std::sin(tilt) + kBackCushion * std::cos(tilt));
    const glm::vec3 center = lowerFrontEdge + up * (0.5f * cushionHeight) - front * (0.5f * kBackCushion);
    glm::mat4 cushionFrame = glm::rotate(glm::mat4(1.0f), -tilt, glm::vec3(1.0f, 0.0f, 0.0f));
    cushionFrame[3] = glm::vec4(center, 1.0f);
    fabric.chamferedBox(glm::vec3(innerWidth - 0.01f, cushionHeight, kBackCushion), kChamfer, cushionFrame);

    // Tapered round legs under the corners.
    if (bottom > 0.0f) {
        MeshBuilder& wood = out.part("wood");
        for (const float x : {-halfWidth + kLegInset, halfWidth - kLegInset}) {
            for (const float z : {-halfDepth + kLegInset, halfDepth - kLegInset}) {
                wood.frustum(0.016f, 0.024f, bottom, kLegSegments, true, false,
                             translation(glm::vec3(x, 0.5f * bottom, z)));
            }
        }
    }

    out.addSocket("Seat", SocketKind::Anchor,
                  translation(glm::vec3(0.0f, p.seatHeight, 0.5f * (backFront + seatFront))));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildArmchair(const ArmchairParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
