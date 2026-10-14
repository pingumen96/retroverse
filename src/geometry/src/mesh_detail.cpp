#include "mesh_detail.hpp"

#include <array>
#include <cmath>
#include <cstddef>

namespace rv::geometry::detail {

glm::vec3 normalizeOr(const glm::vec3& v, const glm::vec3& fallback) noexcept {
    const float lengthSquared = glm::dot(v, v);
    if (!(lengthSquared > 1e-30f) || !std::isfinite(lengthSquared)) {
        return fallback;
    }
    return v / std::sqrt(lengthSquared);
}

glm::vec3 anyPerpendicular(const glm::vec3& n) noexcept {
    const glm::vec3 axis = std::fabs(n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    return normalizeOr(glm::cross(axis, n), glm::vec3(0.0f, 0.0f, 1.0f));
}

glm::vec3 orthonormalize(const glm::vec3& v, const glm::vec3& n, const glm::vec3& fallback) noexcept {
    const glm::vec3 projected = v - n * glm::dot(n, v);
    const glm::vec3 projectedFallback = fallback - n * glm::dot(n, fallback);
    return normalizeOr(projected, normalizeOr(projectedFallback, anyPerpendicular(n)));
}

glm::vec4 triangleTangent(const glm::vec3& n, const glm::vec3& e1, const glm::vec3& e2, const glm::vec2& d1,
                          const glm::vec2& d2, const glm::vec3& fallback) noexcept {
    const float determinant = d1.x * d2.y - d2.x * d1.y;
    if (std::fabs(determinant) < kDegenerateUvArea) {
        return glm::vec4(orthonormalize(fallback, n, fallback), -1.0f);
    }
    const float r = 1.0f / determinant;
    const glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r;
    const glm::vec3 b = (e2 * d1.x - e1 * d2.x) * r;
    const glm::vec3 tangent = orthonormalize(t, n, fallback);
    const float handedness = glm::dot(glm::cross(n, tangent), b) < 0.0f ? -1.0f : 1.0f;
    return glm::vec4(tangent, handedness);
}

std::uint32_t addVertex(MeshData& mesh, const glm::vec3& position, const glm::vec3& normal, const glm::vec4& tangent,
                        const glm::vec2& uv) {
    mesh.vertices.push_back(Vertex{position, normal, tangent, uv});
    return static_cast<std::uint32_t>(mesh.vertices.size() - 1);
}

void addTriangle(MeshData& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
}

namespace {

/// Newell's method: robust polygon normal (not normalized) whose direction follows the vertex order.
[[nodiscard]] glm::vec3 newellNormal(std::span<const glm::vec3> points) noexcept {
    glm::vec3 normal(0.0f);
    for (std::size_t i = 0; i < points.size(); ++i) {
        const glm::vec3& a = points[i];
        const glm::vec3& b = points[(i + 1) % points.size()];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    return normal;
}

/// Unit polygon normal on the side of `outward` (normalized `outward` for degenerate polygons).
[[nodiscard]] glm::vec3 orientedNormal(std::span<const glm::vec3> points, const glm::vec3& outward) noexcept {
    const glm::vec3 newell = newellNormal(points);
    const glm::vec3 fallback = normalizeOr(outward, glm::vec3(0.0f, 1.0f, 0.0f));
    return normalizeOr(glm::dot(newell, outward) < 0.0f ? -newell : newell, fallback);
}

} // namespace

void addFlatPolygon(MeshData& mesh, std::span<const glm::vec3> points, std::span<const glm::vec2> uvs,
                    const glm::vec3& outward) {
    const std::size_t count = points.size();
    if (count < 3 || uvs.size() != count) {
        return;
    }
    const bool reversed = glm::dot(newellNormal(points), outward) < 0.0f;
    const glm::vec3 normal = orientedNormal(points, outward);
    const auto at = [&](std::size_t i) {
        return reversed ? count - 1 - i : i;
    };

    // Tangent from the first fan triangle with usable uvs (a planar polygon with an affine uv layout has one frame).
    glm::vec4 tangent(orthonormalize(points[at(1)] - points[at(0)], normal, glm::vec3(1.0f, 0.0f, 0.0f)), -1.0f);
    for (std::size_t i = 1; i + 1 < count; ++i) {
        const glm::vec3 e1 = points[at(i)] - points[at(0)];
        const glm::vec3 e2 = points[at(i + 1)] - points[at(0)];
        const glm::vec2 d1 = uvs[at(i)] - uvs[at(0)];
        const glm::vec2 d2 = uvs[at(i + 1)] - uvs[at(0)];
        if (std::fabs(d1.x * d2.y - d2.x * d1.y) >= kDegenerateUvArea) {
            tangent = triangleTangent(normal, e1, e2, d1, d2, glm::vec3(tangent));
            break;
        }
    }

    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (std::size_t i = 0; i < count; ++i) {
        addVertex(mesh, points[at(i)], normal, tangent, uvs[at(i)]);
    }
    for (std::size_t i = 1; i + 1 < count; ++i) {
        addTriangle(mesh, first, first + static_cast<std::uint32_t>(i), first + static_cast<std::uint32_t>(i + 1));
    }
}

void addPlanarPolygon(MeshData& mesh, std::span<const glm::vec3> points, const glm::vec3& outward) {
    constexpr std::size_t kMaxPoints = 16;
    if (points.size() < 3 || points.size() > kMaxPoints) {
        return;
    }
    const glm::vec3 n = orientedNormal(points, outward);
    glm::vec3 imageUp(0.0f, 1.0f, 0.0f);
    if (std::fabs(n.y) >= 0.9f) {
        imageUp = n.y > 0.0f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 0.0f, 1.0f);
    }
    const glm::vec3 right = normalizeOr(glm::cross(imageUp, n), anyPerpendicular(n));
    const glm::vec3 up = glm::cross(n, right);
    std::array<glm::vec2, kMaxPoints> uvs{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        uvs[i] = glm::vec2(glm::dot(points[i], right), -glm::dot(points[i], up));
    }
    addFlatPolygon(mesh, points, std::span<const glm::vec2>(uvs.data(), points.size()), outward);
}

} // namespace rv::geometry::detail
