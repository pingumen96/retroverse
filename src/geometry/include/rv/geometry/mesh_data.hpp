#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/expected.hpp>

namespace rv::geometry {

/// Standard vertex used by every mesh in Retroverse (procedural and glTF): 48 bytes.
/// Conventions (docs/decisions/ADR-0002): meters, +Y up, right-handed, counter-clockwise front faces, uv0 origin at
/// the top-left of the image (glTF convention). Tangent space as in glTF: tangent.xyz follows +u and
/// bitangent = cross(normal, tangent.xyz) * tangent.w (w = +1 or -1) follows -v, towards the top of the image, so
/// that the green channel of normal maps ("+Y up") tilts normals towards the top of the image. An unmirrored layout
/// (u to the right, v downwards, seen from the front) has w = +1.
struct Vertex {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec4 tangent{1.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 uv0{0.0f};
};
static_assert(sizeof(Vertex) == 48, "Vertex layout is shared with shaders and GPU buffers");

/// Describes one vertex attribute for the GPU upload code (float components only).
struct VertexAttribute {
    std::uint32_t location = 0;
    std::uint32_t components = 0;
    std::uint32_t offset = 0;
};

/// Attribute locations: 0 position, 1 normal, 2 tangent, 3 uv0. Locations 4..7 are reserved for the
/// per-instance model matrix.
struct VertexLayout {
    std::array<VertexAttribute, 4> attributes{};
    std::uint32_t stride = 0;

    [[nodiscard]] static const VertexLayout& standard() noexcept;
};

/// CPU-side triangle mesh (indexed triangle list).
struct MeshData {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    core::Aabb bounds;

    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }
    [[nodiscard]] bool empty() const noexcept { return vertices.empty() || indices.empty(); }
};

/// Recomputes `mesh.bounds` from the vertex positions.
void computeBounds(MeshData& mesh) noexcept;

/// Recomputes tangents from positions, normals and uv0 (per-triangle accumulation, Gram-Schmidt
/// orthogonalization against the normal, handedness in w such that cross(normal, tangent) * w follows -dP/dv: the
/// glTF tangent space of Vertex). Degenerate UVs fall back to an arbitrary tangent perpendicular to the normal.
void computeTangents(MeshData& mesh);

/// Average world length, in meters, of one unit of uv0 along u (x) and along v (y) over the mesh's triangles,
/// weighted by triangle area. The ratio x / y is the width / height an image needs to appear undistorted on the
/// surface (a 0.4 x 0.3 m quad with uv 0..1 gives (0.4, 0.3)). Triangles with degenerate uvs, zero area or
/// non-finite data are skipped; (0, 0) when none is left.
[[nodiscard]] glm::vec2 uvMetersPerUnit(const MeshData& mesh) noexcept;

/// Same for the mesh as drawn with the local -> world `transform`: only its linear part (rotation, scale, shear)
/// matters, so the quad above scaled by (2, 1, 1) gives (0.8, 0.3) and turned by any angle still (0.4, 0.3).
[[nodiscard]] glm::vec2 uvMetersPerUnit(const MeshData& mesh, const glm::mat4& transform) noexcept;

/// Structural validation: index count multiple of 3, indices in range, finite attributes, non-zero normals.
[[nodiscard]] Status validateMesh(const MeshData& mesh);

/// Deterministic 64-bit FNV-1a hash over vertex and index bytes (determinism tests, caches).
[[nodiscard]] std::uint64_t hashMesh(const MeshData& mesh) noexcept;

} // namespace rv::geometry
