// I6 step 3b's showcase (the optical era's, the 90s collector's and the handheld archive's sideboards, ADR-0014 §1
// amended): a glass vitrine on a closed two-door base, a generic console or an upright handheld on a plinth inside.

#include <algorithm>
#include <string>

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

constexpr std::string_view kKind = "showcase";
constexpr float kPost = 0.02f;
constexpr float kCap = 0.03f;
constexpr float kPane = 0.004f;
constexpr float kBackPanel = 0.01f;
constexpr float kFront = 0.018f;
constexpr float kReveal = 0.004f;
constexpr float kPlinthToe = 0.05f;
/// The base's carcass stands behind its doors and their knobs.
constexpr float kFrontRoom = 0.04f;
constexpr float kKnobRadius = 0.01f;
/// The covered plinth inside the vitrine.
constexpr float kPlinthHeight = 0.05f;
/// The generic console's size (its body), the upright handheld's.
constexpr glm::vec3 kConsole{0.26f, 0.055f, 0.19f};
constexpr glm::vec3 kHandheld{0.09f, 0.15f, 0.028f};
constexpr float kHandheldLeanDeg = 12.0f;
constexpr BoxUv kGrain = BoxUv::GrainMeters;
constexpr float kWoodTile = detail::kWoodMetersPerTile;

Status validate(const ShowcaseParams& p) {
    if (Status status = firstError({
            checkRange(kKind, "width", p.width, 0.4f, 2.0f),
            checkRange(kKind, "height", p.height, 0.6f, 2.0f),
            checkRange(kKind, "depth", p.depth, 0.25f, 1.0f),
        });
        !status) {
        return status;
    }
    if (!(p.baseHeight >= 0.15f && p.baseHeight <= p.height - 0.25f)) {
        return outOfRange(kKind, "baseHeight",
                          formatNumber(p.baseHeight) + " is outside [0.15, " + formatNumber(p.height - 0.25f) + "]");
    }
    return ok();
}

/// A generic console of original design on the plinth (y = 0 its top), scaled by `scale`: a low body, a raised deck
/// with a slot, two buttons and two ports on its front.
void addConsole(detail::PrefabAssembler& out, const glm::mat4& at, float scale) {
    const glm::mat4 frame = glm::scale(at, glm::vec3(scale));
    out.part("plastic_body")
        .chamferedBox(kConsole, 0.008f, frame * translation(glm::vec3(0.0f, 0.5f * kConsole.y, 0.0f)))
        .chamferedBox(glm::vec3(0.15f, 0.025f, 0.1f), 0.006f,
                      frame * translation(glm::vec3(0.0f, kConsole.y + 0.0125f, -0.035f)));
    MeshBuilder& dark = out.part("plastic_dark");
    dark.box(glm::vec3(0.11f, 0.004f, 0.022f), frame * translation(glm::vec3(0.0f, kConsole.y + 0.026f, -0.035f)));
    for (const float x : {-0.05f, 0.05f}) {
        dark.box(glm::vec3(0.03f, 0.016f, 0.004f),
                 frame * translation(glm::vec3(x, 0.022f, 0.5f * kConsole.z + 0.0015f)));
    }
    for (const float x : {-0.095f, -0.07f}) {
        out.part("plastic_accent")
            .box(glm::vec3(0.018f, 0.006f, 0.014f), frame * translation(glm::vec3(x, kConsole.y + 0.003f, 0.05f)));
    }
}

/// An upright handheld of original design leaning back on a small stand (y = 0 the plinth's top), scaled by `scale`:
/// its body, a dark screen, a cross pad and two buttons.
void addHandheld(detail::PrefabAssembler& out, const glm::mat4& at, float scale) {
    const glm::mat4 frame = glm::scale(at, glm::vec3(scale));
    MeshBuilder& dark = out.part("plastic_dark");
    dark.chamferedBox(glm::vec3(0.07f, 0.02f, 0.06f), 0.004f, frame * translation(glm::vec3(0.0f, 0.01f, -0.005f)));
    const float lean = core::degreesToRadians(kHandheldLeanDeg);
    glm::mat4 body = glm::rotate(frame * translation(glm::vec3(0.0f, 0.02f, 0.0f)), -lean, glm::vec3(1, 0, 0));
    body = body * translation(glm::vec3(0.0f, 0.5f * kHandheld.y, 0.0f));
    out.part("plastic_body").chamferedBox(kHandheld, 0.008f, body);
    const float face = 0.5f * kHandheld.z + 0.001f;
    dark.box(glm::vec3(0.064f, 0.056f, 0.002f), body * translation(glm::vec3(0.0f, 0.035f, face)));
    dark.box(glm::vec3(0.026f, 0.008f, 0.004f), body * translation(glm::vec3(-0.022f, -0.035f, face)));
    dark.box(glm::vec3(0.008f, 0.026f, 0.004f), body * translation(glm::vec3(-0.022f, -0.035f, face)));
    for (const glm::vec2 button : {glm::vec2(0.018f, -0.03f), glm::vec2(0.03f, -0.04f)}) {
        out.part("plastic_accent")
            .box(glm::vec3(0.01f, 0.01f, 0.004f), body * translation(glm::vec3(button.x, button.y, face)));
    }
}

ProceduralPrefab assemble(const ShowcaseParams& p) {
    detail::PrefabAssembler out(kKind, {"wood_primary", "wood_secondary", "metal_trim", "fabric", "glass",
                                        "plastic_body", "plastic_dark", "plastic_accent"});
    MeshBuilder& wood = out.part("wood_primary");
    MeshBuilder& fronts = out.part("wood_secondary");
    MeshBuilder& metal = out.part("metal_trim");
    MeshBuilder& glass = out.part("glass");

    const float halfWidth = 0.5f * p.width;
    const float halfDepth = 0.5f * p.depth;
    // The base: a carcass with a top over its full size, two doors with knobs, a recessed toe.
    wood.chamferedBox(glm::vec3(p.width, kCap, p.depth), 0.003f,
                      translation(glm::vec3(0.0f, p.baseHeight - 0.5f * kCap, 0.0f)), kGrain, kWoodTile, FaceMask::PosY);
    const float carcassFront = halfDepth - kFrontRoom;
    const float carcassHeight = p.baseHeight - kCap;
    wood.chamferedBox(glm::vec3(p.width - 0.02f, carcassHeight, carcassFront + halfDepth), 0.002f,
                      translation(glm::vec3(0.0f, 0.5f * carcassHeight, 0.5f * (carcassFront - halfDepth))), kGrain,
                      kWoodTile);
    const float doorBottom = std::min(kPlinthToe, 0.3f * carcassHeight);
    const float doorTop = carcassHeight - kReveal;
    if (doorTop - doorBottom > 0.05f) {
        const float doorWidth = 0.5f * (p.width - 0.02f) - 1.5f * kReveal;
        for (const float side : {-1.0f, 1.0f}) {
            const float x = side * (0.5f * kReveal + 0.5f * doorWidth);
            fronts.chamferedBox(glm::vec3(doorWidth, doorTop - doorBottom, kFront), 0.003f,
                                translation(glm::vec3(x, 0.5f * (doorBottom + doorTop), carcassFront + 0.5f * kFront)),
                                kGrain, kWoodTile);
            metal.uvSphere(kKnobRadius, 12, 6,
                           translation(glm::vec3(side * 0.05f, 0.5f * (doorBottom + doorTop) + 0.1f * (doorTop - doorBottom),
                                                 carcassFront + kFront + kKnobRadius)));
        }
    }

    // The vitrine: four posts under a cap, a back panel, the glass front, sides and top.
    const float vitrineHeight = p.height - p.baseHeight - kCap;
    const float postY = p.baseHeight + 0.5f * vitrineHeight;
    for (const float x : {-halfWidth + 0.5f * kPost, halfWidth - 0.5f * kPost}) {
        for (const float z : {-halfDepth + 0.5f * kPost, halfDepth - 0.5f * kPost}) {
            wood.box(glm::vec3(kPost, vitrineHeight, kPost), translation(glm::vec3(x, postY, z)), kGrain, kWoodTile);
        }
    }
    wood.chamferedBox(glm::vec3(p.width, kCap, p.depth), 0.004f,
                      translation(glm::vec3(0.0f, p.height - 0.5f * kCap, 0.0f)), kGrain, kWoodTile);
    const float innerWidth = p.width - 2.0f * kPost;
    const float innerDepth = p.depth - 2.0f * kPost;
    fronts.box(glm::vec3(innerWidth, vitrineHeight, kBackPanel),
               translation(glm::vec3(0.0f, postY, -halfDepth + 0.5f * kBackPanel)), kGrain, kWoodTile);
    glass.box(glm::vec3(innerWidth, vitrineHeight, kPane),
              translation(glm::vec3(0.0f, postY, halfDepth - 0.5f * kPost)), BoxUv::PerFace, 1.0f,
              FaceMask::PosZ);
    for (const float side : {-1.0f, 1.0f}) {
        glass.box(glm::vec3(kPane, vitrineHeight, innerDepth),
                  translation(glm::vec3(side * (halfWidth - 0.5f * kPost), postY, 0.0f)), BoxUv::PerFace, 1.0f,
                  side > 0.0f ? FaceMask::PosX : FaceMask::NegX);
    }

    // The plinth and the item on it.
    const float plinthWidth = std::min(0.6f * innerWidth, 0.4f);
    const float plinthDepth = std::min(0.7f * innerDepth, 0.3f);
    const float floorY = p.baseHeight;
    out.part("fabric").chamferedBox(glm::vec3(plinthWidth, kPlinthHeight, plinthDepth), 0.006f,
                                    translation(glm::vec3(0.0f, floorY + 0.5f * kPlinthHeight, 0.0f)));
    const float plinthTop = floorY + kPlinthHeight;
    const glm::mat4 itemFrame = translation(glm::vec3(0.0f, plinthTop, 0.0f));
    const float room = p.height - kCap - plinthTop - 0.02f;
    if (p.item == ShowcaseItem::Console) {
        const float scale = std::min({1.0f, (plinthWidth - 0.02f) / kConsole.x, (plinthDepth - 0.02f) / kConsole.z,
                                      room / (kConsole.y + 0.03f)});
        addConsole(out, itemFrame, scale);
    } else if (p.item == ShowcaseItem::Handheld) {
        const float scale = std::min({1.0f, room / (kHandheld.y + 0.03f), (plinthDepth - 0.02f) / 0.08f});
        addHandheld(out, itemFrame, scale);
    }
    out.addSocket("Item", SocketKind::Anchor, itemFrame);
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildShowcase(const ShowcaseParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
