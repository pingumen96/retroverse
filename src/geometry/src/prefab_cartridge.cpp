#include <algorithm>
#include <cmath>
#include <string>

#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::translation;

constexpr std::string_view kKind = "cartridge";
/// Grip grooves: one rib plus one groove per pitch; fewer than two ribs leave a plain border.
constexpr float kRibPitch = 0.0028f;
constexpr int kMaxRibs = 8;
/// Smallest label edge (meters).
constexpr float kMinLabel = 0.004f;

/// How far the shell's top face lies below the nominal top: a top spine label takes the last 0.5 mm.
[[nodiscard]] float topDrop(const CartridgeParams& p) noexcept {
    return p.spineLabel && p.spineFace == SpineFace::Top ? detail::kDecalOffset : 0.0f;
}

/// How far the shell's -X face lies inside the nominal side: a side spine label takes the last 0.5 mm.
[[nodiscard]] float sideDrop(const CartridgeParams& p) noexcept {
    return p.spineLabel && p.spineFace == SpineFace::Side ? detail::kDecalOffset : 0.0f;
}

/// Depth of the front label pocket: labelInset to the label, 0.5 mm more to the pocket floor.
[[nodiscard]] float pocketDepthOf(const CartridgeParams& p) noexcept {
    return p.labelInset + detail::kDecalOffset;
}

/// Rotation of the spine quad and its socket: +X along the item's +Z, +Y along its +X, +Z (the normal) along its +Y.
[[nodiscard]] glm::mat4 spineFrame(const glm::vec3& position) {
    glm::mat4 frame(1.0f);
    frame[0] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    frame[1] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    frame[2] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    frame[3] = glm::vec4(position, 1.0f);
    return frame;
}

/// Rotation of a side spine quad and its socket: +X along the item's +Z, +Y along its +Y, +Z (the normal) along its -X.
[[nodiscard]] glm::mat4 sideSpineFrame(const glm::vec3& position) {
    glm::mat4 frame(1.0f);
    frame[0] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    frame[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    frame[2] = glm::vec4(-1.0f, 0.0f, 0.0f, 0.0f);
    frame[3] = glm::vec4(position, 1.0f);
    return frame;
}

Status validate(const CartridgeParams& p) {
    const glm::vec3 s = p.size;
    const float smallest = std::min({s.x, s.y, s.z});
    if (Status status = firstError({
            checkRange(kKind, "size", s, glm::vec3(0.02f, 0.02f, 0.004f), glm::vec3(0.5f, 0.5f, 0.2f)),
            checkRange(kKind, "chamfer", p.chamfer, 0.0f, 0.25f * smallest),
            checkRange(kKind, "labelMargin", p.labelMargin,
                       glm::vec2(p.chamfer + detail::kDecalOffset + sideDrop(p),
                                 p.chamfer + detail::kDecalOffset + topDrop(p)),
                       glm::vec2(0.5f * (s.x - kMinLabel), 0.5f * (s.y - kMinLabel))),
            checkRange(kKind, "labelInset", p.labelInset, detail::kDecalOffset, std::min(0.005f, 0.25f * s.z)),
        });
        !status) {
        return status;
    }
    if (p.spineLabel) {
        // The label stays on the flat part of its face (the top, or the -X side): inside the chamfers and behind the
        // front pocket.
        const float flatAcross = s.z - pocketDepthOf(p) - 2.0f * p.chamfer;
        const float flatAlong = (p.spineFace == SpineFace::Side ? s.y : s.x) - 2.0f * p.chamfer;
        if (Status status = checkRange(kKind, "spineLabelSize", p.spineLabelSize, glm::vec2(kMinLabel),
                                       glm::vec2(flatAcross, flatAlong));
            !status) {
            return status;
        }
    }
    return ok();
}

ProceduralPrefab assemble(const CartridgeParams& p) {
    detail::PrefabAssembler out(kKind, {"plastic_shell", "label", "spine"});
    MeshBuilder& shell = out.part("plastic_shell");

    const glm::vec3 half = 0.5f * p.size;
    const float c = p.chamfer;
    const float front = half.z;
    // The label lies labelInset behind the front; the pocket floor (front face of the body) another 0.5 mm deeper,
    // so no shell face is coplanar with the label.
    const float labelZ = front - p.labelInset;
    const float pocketDepth = pocketDepthOf(p);
    const float pocket = front - pocketDepth;
    // With a top spine label the shell's top face lies 0.5 mm below the nominal top, the label on it; with a side one
    // its -X face lies 0.5 mm inside the nominal side.
    const float drop = topDrop(p);
    const float dropX = sideDrop(p);
    const float top = half.y - drop;
    const float left = -half.x + dropX;

    // Body: everything behind the pocket floor.
    shell.chamferedBox(glm::vec3(p.size.x - dropX, p.size.y - drop, p.size.z - pocketDepth), c,
                       translation(glm::vec3(0.5f * dropX, -0.5f * drop, -0.5f * pocketDepth)));

    // Raised border around the label pocket, on the flat part of the body's front face. Its back faces rest on
    // the body and are dropped.
    const FaceMask raised = FaceMask::All & ~FaceMask::NegZ;
    const auto addBorder = [&](float x0, float x1, float y0, float y1) {
        shell.box(glm::vec3(x1 - x0, y1 - y0, pocketDepth),
                  translation(glm::vec3(0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (pocket + front))), BoxUv::PerFace,
                  1.0f, raised);
    };
    const float flatX = half.x - c;
    const float flatLeft = left + c;
    const float flatTop = top - c;
    const float flatBottom = -half.y + c;
    const float labelX = half.x - p.labelMargin.x;
    const float labelY = half.y - p.labelMargin.y;
    addBorder(flatLeft, -labelX, flatBottom, flatTop); // left
    addBorder(labelX, flatX, flatBottom, flatTop);     // right
    addBorder(-labelX, labelX, labelY, flatTop);     // top
    const float band = -labelY - flatBottom;         // bottom band under the label
    const int ribs = std::min(kMaxRibs, static_cast<int>(band / kRibPitch));
    if (p.gripRidges && ribs >= 2) {
        // Ribs and grooves of equal height: the lowest rib touches the bottom edge, the highest one the label.
        const float rib = band / static_cast<float>(2 * ribs - 1);
        for (int i = 0; i < ribs; ++i) {
            const float y0 = flatBottom + static_cast<float>(2 * i) * rib;
            addBorder(-labelX, labelX, y0, y0 + rib);
        }
    } else {
        addBorder(-labelX, labelX, flatBottom, -labelY);
    }

    // Label: recessed by labelInset, 0.5 mm above the pocket floor.
    const glm::vec2 labelSize(2.0f * labelX, 2.0f * labelY);
    out.part("label").quad(labelSize, translation(glm::vec3(0.0f, 0.0f, labelZ)));
    out.addSocket("Label", SocketKind::Other, translation(glm::vec3(0.0f, 0.0f, labelZ)),
                  core::Aabb::fromMinMax(glm::vec3(-labelX, -labelY, 0.0f), glm::vec3(labelX, labelY, 0.0f)));

    // Spine: on the flat part of the top face (or of the -X face), between the chamfers, behind the pocket, 0.5 mm
    // outside the shell.
    if (p.spineLabel) {
        const float centerZ = -0.5f * pocketDepth; // middle of the body's depth
        const glm::mat4 frame = p.spineFace == SpineFace::Side ? sideSpineFrame(glm::vec3(-half.x, 0.0f, centerZ))
                                                               : spineFrame(glm::vec3(0.0f, half.y, centerZ));
        const glm::vec2 spine = p.spineLabelSize; // (across, along) = the quad's (x, y)
        out.part("spine").quad(spine, frame);
        out.addSocket("Spine", SocketKind::Other, frame,
                      core::Aabb::fromMinMax(glm::vec3(-0.5f * spine.x, -0.5f * spine.y, 0.0f),
                                             glm::vec3(0.5f * spine.x, 0.5f * spine.y, 0.0f)));
    }
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildCartridge(const CartridgeParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

glm::vec3 slotSizeFor(SlotFacing facing, const glm::vec3& itemSize) noexcept {
    switch (facing) {
    case SlotFacing::SpineOut:
        return glm::vec3(itemSize.z, itemSize.x, itemSize.y);
    case SlotFacing::SideOut:
        return glm::vec3(itemSize.z, itemSize.y, itemSize.x);
    case SlotFacing::FaceOut:
        break;
    }
    return itemSize;
}

glm::mat4 slotItemTransform(SlotFacing facing, const glm::vec3& itemSize) noexcept {
    if (facing == SlotFacing::FaceOut) {
        return translation(glm::vec3(0.0f, 0.5f * itemSize.y, 0.0f));
    }
    if (facing == SlotFacing::SideOut) {
        // A quarter turn about +Y: the item's -X (its side spine) out (+Z), its +Z (front) to the slot's +X, upright.
        glm::mat4 transform(1.0f);
        transform[0] = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
        transform[1] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
        transform[2] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
        transform[3] = glm::vec4(0.0f, 0.5f * itemSize.y, 0.0f, 1.0f);
        return transform;
    }
    // On its end: the item's +X up (the slot's +Y), its +Y (top face) out (+Z), its +Z (front) to the slot's +X.
    glm::mat4 transform(1.0f);
    transform[0] = glm::vec4(0.0f, 1.0f, 0.0f, 0.0f);
    transform[1] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
    transform[2] = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    transform[3] = glm::vec4(0.0f, 0.5f * itemSize.x, 0.0f, 1.0f);
    return transform;
}

} // namespace rv::geometry
