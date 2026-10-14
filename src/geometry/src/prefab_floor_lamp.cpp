#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include <rv/core/math.hpp>
#include <rv/geometry/prefabs.hpp>

#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;
using detail::translation;

constexpr std::string_view kKind = "floor_lamp";
/// Divisible by 4 so the round parts reach their radius exactly on the X and Z axes (exact bounds).
constexpr int kSegments = 48;
constexpr float kBaseThickness = 0.022f;
constexpr float kPoleRadius = 0.011f;
constexpr float kHolderRadius = 0.017f;
constexpr float kHolderHeight = 0.045f;
/// Shade top radius relative to its bottom radius.
constexpr float kShadeTopRatio = 0.72f;
/// The shade is a thin closed shell: its inner surface sits this far inside the outer one, so it renders from
/// inside and outside whatever the material's culling mode (no coincident opposite faces).
constexpr float kShadeWall = 0.002f;
constexpr int kSpiderArms = 3;

Status validate(const FloorLampParams& p) {
    return firstError({
        checkRange(kKind, "height", p.height, 0.5f, 3.0f),
        checkRange(kKind, "baseRadius", p.baseRadius, 0.05f, 1.0f),
        checkRange(kKind, "shadeRadius", p.shadeRadius, 0.05f, 1.0f),
        checkRange(kKind, "shadeHeight", p.shadeHeight, 0.05f, std::min(1.0f, 0.5f * p.height)),
    });
}

/// The same surface seen from the other side: inverted normals and winding.
[[nodiscard]] MeshData insideOut(MeshData mesh) {
    for (Vertex& v : mesh.vertices) {
        v.normal = -v.normal;
        v.tangent.w = -v.tangent.w;
    }
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
    }
    return mesh;
}

ProceduralPrefab assemble(const FloorLampParams& p) {
    detail::PrefabAssembler out(kKind, {"metal", "lamp_shade"});
    MeshBuilder& metal = out.part("metal");

    // Weighted disc base with a small boss holding the pole.
    const float baseTop = 0.82f * p.baseRadius;
    metal.frustum(p.baseRadius, baseTop, kBaseThickness, kSegments, true, false,
                  translation(glm::vec3(0.0f, 0.5f * kBaseThickness, 0.0f)));
    const float bossRadius = std::min(0.045f, 0.7f * baseTop);
    const float bossHeight = 0.03f;
    metal.frustum(bossRadius, 0.6f * bossRadius, bossHeight, kSegments / 2, true, false,
                  translation(glm::vec3(0.0f, kBaseThickness + 0.5f * bossHeight, 0.0f)));

    // Shade on top; the bulb holder hangs on a three-arm spider inside it.
    const float shadeBottom = p.height - p.shadeHeight;
    const float armY = p.height - 0.62f * p.shadeHeight;
    const float holderBottom = armY - kHolderHeight + 0.005f;
    const float poleBottom = kBaseThickness + bossHeight;
    metal.cylinder(kPoleRadius, holderBottom - poleBottom, 16, false,
                   translation(glm::vec3(0.0f, 0.5f * (poleBottom + holderBottom), 0.0f)));
    metal.cylinder(kHolderRadius, kHolderHeight, 24, true,
                   translation(glm::vec3(0.0f, holderBottom + 0.5f * kHolderHeight, 0.0f)));
    const float shadeRadiusAtArms =
        p.shadeRadius + (kShadeTopRatio - 1.0f) * p.shadeRadius * (armY - shadeBottom) / p.shadeHeight;
    const float armLength = shadeRadiusAtArms - kShadeWall - 0.002f - kHolderRadius;
    const float armCenter = kHolderRadius + 0.5f * armLength;
    for (int i = 0; i < kSpiderArms; ++i) {
        const float angle = core::kTwoPi * static_cast<float>(i) / static_cast<float>(kSpiderArms);
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        glm::mat4 arm(1.0f);
        arm[0] = glm::vec4(c, 0.0f, -s, 0.0f);
        arm[2] = glm::vec4(s, 0.0f, c, 0.0f);
        arm[3] = glm::vec4(c * armCenter, armY, -s * armCenter, 1.0f);
        metal.box(glm::vec3(armLength, 0.004f, 0.006f), arm);
    }

    // Shade: outer surface, inner surface turned inwards, and the two rims closing the wall.
    const float shadeTop = kShadeTopRatio * p.shadeRadius;
    const glm::mat4 shadeFrame = translation(glm::vec3(0.0f, p.height - 0.5f * p.shadeHeight, 0.0f));
    MeshBuilder inner;
    inner.frustum(p.shadeRadius - kShadeWall, shadeTop - kShadeWall, p.shadeHeight, kSegments, false, false,
                  shadeFrame);
    out.part("lamp_shade")
        .frustum(p.shadeRadius, shadeTop, p.shadeHeight, kSegments, false, false, shadeFrame)
        .append(insideOut(std::move(inner).build("inner")))
        .frustum(shadeTop, shadeTop - kShadeWall, 0.0f, kSegments, false, false,
                 translation(glm::vec3(0.0f, p.height, 0.0f)))
        .frustum(p.shadeRadius - kShadeWall, p.shadeRadius, 0.0f, kSegments, false, false,
                 translation(glm::vec3(0.0f, shadeBottom, 0.0f)));

    const float bulbY = std::min(armY + 0.04f, p.height - 0.2f * p.shadeHeight);
    out.addSocket("LightSource", SocketKind::Anchor, translation(glm::vec3(0.0f, bulbY, 0.0f)));
    return std::move(out).finish();
}

} // namespace

Expected<ProceduralPrefab> buildFloorLamp(const FloorLampParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    return assemble(params);
}

} // namespace rv::geometry
