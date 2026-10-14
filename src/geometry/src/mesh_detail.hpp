#pragma once

// Internal helpers shared by the mesh builder and the procedural prefabs. Not installed, not part of the API.

#include <cstdint>
#include <span>

#include <glm/glm.hpp>

#include <rv/geometry/mesh_data.hpp>

namespace rv::geometry::detail {

/// Same threshold as computeTangents: uv triangles with a smaller signed area carry no tangent information.
inline constexpr float kDegenerateUvArea = 1e-12f;

/// `v / |v|`, or `fallback` when |v| is too small to normalize.
[[nodiscard]] glm::vec3 normalizeOr(const glm::vec3& v, const glm::vec3& fallback) noexcept;

/// Some unit vector perpendicular to the unit vector `n`.
[[nodiscard]] glm::vec3 anyPerpendicular(const glm::vec3& n) noexcept;

/// Unit vector along `v` minus its component along the unit vector `n`; falls back to `fallback` treated the
/// same way, then to any perpendicular of `n`.
[[nodiscard]] glm::vec3 orthonormalize(const glm::vec3& v, const glm::vec3& n, const glm::vec3& fallback) noexcept;

/// Tangent frame of a triangle (computeTangents' formula): edges e1 = p1 - p0, e2 = p2 - p0 and uv deltas d1, d2.
/// Returns (unit tangent orthogonal to `n`, handedness). Degenerate uvs give (`fallback` orthonormalized, -1),
/// the handedness of every Retroverse uv layout (v grows downwards).
[[nodiscard]] glm::vec4 triangleTangent(const glm::vec3& n, const glm::vec3& e1, const glm::vec3& e2,
                                        const glm::vec2& d1, const glm::vec2& d2, const glm::vec3& fallback) noexcept;

/// Appends a vertex and returns its index.
std::uint32_t addVertex(MeshData& mesh, const glm::vec3& position, const glm::vec3& normal, const glm::vec4& tangent,
                        const glm::vec2& uv);

void addTriangle(MeshData& mesh, std::uint32_t a, std::uint32_t b, std::uint32_t c);

/// Appends a flat convex planar polygon (fan triangulation, 3 to N points with matching uvs). The polygon is wound
/// counter-clockwise as seen from the side `outward` points to (the given order is reversed when needed) and gets
/// its geometric normal oriented the same way. `outward` only needs to lie on the outer side of the plane.
void addFlatPolygon(MeshData& mesh, std::span<const glm::vec3> points, std::span<const glm::vec2> uvs,
                    const glm::vec3& outward);

/// addFlatPolygon with planar uvs in meters, oriented like BoxUv::WorldMeters: seen from outside, u grows to the
/// right and v downwards, where "up" is +Y (-Z for faces looking up, +Z for faces looking down).
void addPlanarPolygon(MeshData& mesh, std::span<const glm::vec3> points, const glm::vec3& outward);

} // namespace rv::geometry::detail
