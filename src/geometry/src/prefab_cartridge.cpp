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

Status validate(const CartridgeParams& p) {
    const glm::vec3 s = p.size;
    const float smallest = std::min({s.x, s.y, s.z});
    return firstError({
        checkRange(kKind, "size", s, glm::vec3(0.02f, 0.02f, 0.004f), glm::vec3(0.5f, 0.5f, 0.2f)),
        checkRange(kKind, "chamfer", p.chamfer, 0.0f, 0.25f * smallest),
        checkRange(kKind, "labelMargin", p.labelMargin, glm::vec2(p.chamfer + detail::kDecalOffset),
                   glm::vec2(0.5f * (s.x - kMinLabel), 0.5f * (s.y - kMinLabel))),
        checkRange(kKind, "labelInset", p.labelInset, detail::kDecalOffset, std::min(0.005f, 0.25f * s.z)),
    });
}

ProceduralPrefab assemble(const CartridgeParams& p) {
    detail::PrefabAssembler out(kKind, {"plastic_shell", "label"});
    MeshBuilder& shell = out.part("plastic_shell");

    const glm::vec3 half = 0.5f * p.size;
    const float c = p.chamfer;
    const float front = half.z;
    // The label lies labelInset behind the front; the pocket floor (front face of the body) another 0.5 mm deeper,
    // so no shell face is coplanar with the label.
    const float labelZ = front - p.labelInset;
    const float pocketDepth = p.labelInset + detail::kDecalOffset;
    const float pocket = front - pocketDepth;

    // Body: everything behind the pocket floor.
    shell.chamferedBox(glm::vec3(p.size.x, p.size.y, p.size.z - pocketDepth), c,
                       translation(glm::vec3(0.0f, 0.0f, -0.5f * pocketDepth)));

    // Raised border around the label pocket, on the flat part of the body's front face. Its back faces rest on
    // the body and are dropped.
    const FaceMask raised = FaceMask::All & ~FaceMask::NegZ;
    const auto addBorder = [&](float x0, float x1, float y0, float y1) {
        shell.box(glm::vec3(x1 - x0, y1 - y0, pocketDepth),
                  translation(glm::vec3(0.5f * (x0 + x1), 0.5f * (y0 + y1), 0.5f * (pocket + front))), BoxUv::PerFace,
                  1.0f, raised);
    };
    const float flatX = half.x - c;
    const float flatY = half.y - c;
    const float labelX = half.x - p.labelMargin.x;
    const float labelY = half.y - p.labelMargin.y;
    addBorder(-flatX, -labelX, -flatY, flatY); // left
    addBorder(labelX, flatX, -flatY, flatY);   // right
    addBorder(-labelX, labelX, labelY, flatY); // top
    const float band = flatY - labelY;         // bottom band under the label
    const int ribs = std::min(kMaxRibs, static_cast<int>(band / kRibPitch));
    if (p.gripRidges && ribs >= 2) {
        // Ribs and grooves of equal height: the lowest rib touches the bottom edge, the highest one the label.
        const float rib = band / static_cast<float>(2 * ribs - 1);
        for (int i = 0; i < ribs; ++i) {
            const float y0 = -flatY + static_cast<float>(2 * i) * rib;
            addBorder(-labelX, labelX, y0, y0 + rib);
        }
    } else {
        addBorder(-labelX, labelX, -flatY, -labelY);
    }

    // Label: recessed by labelInset, 0.5 mm above the pocket floor.
    const glm::vec2 labelSize(2.0f * labelX, 2.0f * labelY);
    out.part("label").quad(labelSize, translation(glm::vec3(0.0f, 0.0f, labelZ)));
    out.addSocket("Label", SocketKind::Other, translation(glm::vec3(0.0f, 0.0f, labelZ)),
                  core::Aabb::fromMinMax(glm::vec3(-labelX, -labelY, 0.0f), glm::vec3(labelX, labelY, 0.0f)));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildCartridge(const CartridgeParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
