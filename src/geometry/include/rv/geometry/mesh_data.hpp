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
/// Conventions (docs/decisions/ADR-0002): meters, +Y up, right-handed, counter-clockwise front faces,
/// tangent.w = handedness (+1 or -1), uv0 origin at the top-left of the image (glTF convention).
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
/// orthogonalization against the normal, handedness in w). Degenerate UVs fall back to an arbitrary
/// tangent perpendicular to the normal.
void computeTangents(MeshData& mesh);

/// Structural validation: index count multiple of 3, indices in range, finite attributes, non-zero normals.
[[nodiscard]] Status validateMesh(const MeshData& mesh);

/// Deterministic 64-bit FNV-1a hash over vertex and index bytes (determinism tests, caches).
[[nodiscard]] std::uint64_t hashMesh(const MeshData& mesh) noexcept;

} // namespace rv::geometry
