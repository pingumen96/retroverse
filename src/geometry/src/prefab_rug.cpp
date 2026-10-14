#include <array>

#include <rv/geometry/prefabs.hpp>

#include "mesh_detail.hpp"
#include "prefab_assembly.hpp"

namespace rv::geometry {
namespace {

using detail::checkRange;
using detail::firstError;

constexpr std::string_view kKind = "rug";

Status validate(const RugParams& p) {
    return firstError({
        checkRange(kKind, "width", p.width, 0.1f, 20.0f),
        checkRange(kKind, "depth", p.depth, 0.1f, 20.0f),
        checkRange(kKind, "thickness", p.thickness, 0.001f, 0.1f),
    });
}

/// Top face with uv 0..1 ((0, 0) at the -X/-Z corner) and four edges whose uvs fold the image border over the
/// edge, like fabric wrapping around. No bottom face: it lies on the floor.
[[nodiscard]] MeshData rugMesh(const RugParams& p) {
    MeshData mesh;
    const float x = 0.5f * p.width;
    const float z = 0.5f * p.depth;
    const float t = p.thickness;
    const float foldU = t / p.width;
    const float foldV = t / p.depth;
    const auto polygon = [&](const std::array<glm::vec3, 4>& points, const std::array<glm::vec2, 4>& uvs,
                             const glm::vec3& outward) {
        detail::addFlatPolygon(mesh, points, uvs, outward);
    };

    polygon({glm::vec3(-x, t, -z), glm::vec3(-x, t, z), glm::vec3(x, t, z), glm::vec3(x, t, -z)},
            {glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 0.0f)},
            glm::vec3(0.0f, 1.0f, 0.0f));
    polygon(
        {glm::vec3(-x, t, z), glm::vec3(-x, 0.0f, z), glm::vec3(x, 0.0f, z), glm::vec3(x, t, z)},
        {glm::vec2(0.0f, 1.0f), glm::vec2(0.0f, 1.0f - foldV), glm::vec2(1.0f, 1.0f - foldV), glm::vec2(1.0f, 1.0f)},
        glm::vec3(0.0f, 0.0f, 1.0f));
    polygon({glm::vec3(x, t, -z), glm::vec3(x, 0.0f, -z), glm::vec3(-x, 0.0f, -z), glm::vec3(-x, t, -z)},
            {glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, foldV), glm::vec2(0.0f, foldV), glm::vec2(0.0f, 0.0f)},
            glm::vec3(0.0f, 0.0f, -1.0f));
    polygon(
        {glm::vec3(x, t, z), glm::vec3(x, 0.0f, z), glm::vec3(x, 0.0f, -z), glm::vec3(x, t, -z)},
        {glm::vec2(1.0f, 1.0f), glm::vec2(1.0f - foldU, 1.0f), glm::vec2(1.0f - foldU, 0.0f), glm::vec2(1.0f, 0.0f)},
        glm::vec3(1.0f, 0.0f, 0.0f));
    polygon({glm::vec3(-x, t, -z), glm::vec3(-x, 0.0f, -z), glm::vec3(-x, 0.0f, z), glm::vec3(-x, t, z)},
            {glm::vec2(0.0f, 0.0f), glm::vec2(foldU, 0.0f), glm::vec2(foldU, 1.0f), glm::vec2(0.0f, 1.0f)},
            glm::vec3(-1.0f, 0.0f, 0.0f));
    return mesh;
}

} // namespace

Expected<ProceduralPrefab> buildRug(const RugParams& params) {
    if (Status status = validate(params); !status) {
        return propagate(status.error());
    }
    detail::PrefabAssembler out(kKind, {"fabric"});
    out.part("fabric").append(rugMesh(params));
    return std::move(out).finish();
}

} // namespace rv::geometry
