#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <rv/geometry/mesh_data.hpp>

namespace rv::geometry {

/// UV layout of MeshBuilder::box faces.
enum class BoxUv {
    /// Every face spans uv [0, 1] with (0, 0) at its top-left corner seen from outside. "Up" is +Y for the side
    /// faces, -Z for the top face (+Y) and +Z for the bottom face (-Y), all in the box's local frame.
    PerFace,
    /// uv = (coordinate along the face's right axis, coordinate along its down axis) of the transformed position,
    /// divided by metersPerTile. Coplanar faces of different boxes (wall segments, floor pieces) therefore tile
    /// seamlessly in builder space.
    WorldMeters,
    /// Meters like WorldMeters, but on every face u runs along the face's longer side (as placed) and v across it,
    /// the way the grain of a board follows its length: wood textures whose grain runs along u stay at one scale on
    /// every board of a piece of furniture. Where a face is taller than wide, uv = (coordinate along its down axis,
    /// coordinate along its left axis): the WorldMeters layout turned a quarter turn, never mirrored.
    GrainMeters,
};

/// Set of box faces, named by their outward normal in the box's local frame.
enum class FaceMask : std::uint8_t {
    None = 0,
    PosX = 1u << 0,
    NegX = 1u << 1,
    PosY = 1u << 2,
    NegY = 1u << 3,
    PosZ = 1u << 4,
    NegZ = 1u << 5,
    All = 0x3Fu,
};

[[nodiscard]] constexpr FaceMask operator|(FaceMask a, FaceMask b) noexcept {
    return static_cast<FaceMask>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

[[nodiscard]] constexpr FaceMask operator&(FaceMask a, FaceMask b) noexcept {
    return static_cast<FaceMask>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
}

/// Complement within FaceMask::All, e.g. `FaceMask::All & ~FaceMask::NegY` drops the bottom face.
[[nodiscard]] constexpr FaceMask operator~(FaceMask a) noexcept {
    return static_cast<FaceMask>(~static_cast<unsigned>(a) & static_cast<unsigned>(FaceMask::All));
}

/// True when `mask` contains at least one face of `faces`.
[[nodiscard]] constexpr bool hasAny(FaceMask mask, FaceMask faces) noexcept {
    return (static_cast<unsigned>(mask) & static_cast<unsigned>(faces)) != 0u;
}

/// Fluent builder that appends procedural primitives into one indexed triangle mesh (GPU-free, deterministic).
///
/// Conventions (docs/decisions/ADR-0002): meters, +Y up, +Z front, counter-clockwise front faces seen from
/// outside, uv (0, 0) at the top-left of the image. Every primitive is defined in its own local frame and placed
/// with `transform`, an affine matrix (its projective row is ignored): positions use the full matrix, normals the
/// inverse-transpose of the upper 3x3 (renormalized), tangents the upper 3x3 (re-orthogonalized against the
/// normal). A mirroring transform (negative determinant) reverses the triangle winding and flips the tangent
/// handedness, so faces stay counter-clockwise and outward.
///
/// Bad input never crashes and never produces NaN: a primitive with a non-finite argument or a non-finite or
/// singular transform is skipped (nothing is appended), and so is one whose placed positions or uvs are not all
/// finite (an appended mesh with non-finite data, a placement beyond the float range); negative sizes, radii and
/// chamfers use their magnitude; segment and ring counts are clamped to [3, 1024] and [2, 512]; a non-positive
/// metersPerTile is treated as 1.
class MeshBuilder {
public:
    /// Axis-aligned box centered on the origin, 4 vertices per face (24 vertices and 36 indices with all faces),
    /// flat outward normals. `faces` drops faces that are never visible (furniture bottoms, faces against walls).
    MeshBuilder& box(const glm::vec3& size, const glm::mat4& transform = glm::mat4(1.0f), BoxUv uv = BoxUv::PerFace,
                     float metersPerTile = 1.0f, FaceMask faces = FaceMask::All);

    /// Box centered on the origin whose 12 edges are bevelled by `chamfer` meters (measured along each face) with
    /// flat-shaded 45-degree strips and corner triangles (96 vertices, 132 indices). The chamfer is clamped to 45%
    /// of the smallest dimension; a chamfer below 0.01 mm produces a plain box (with the same uv layout). UVs follow
    /// `uv` over the full box faces (BoxUv::PerFace: each face spans 0..1; the meter layouts divide by
    /// metersPerTile); bevels reuse the mapping of the face they lean towards.
    MeshBuilder& chamferedBox(const glm::vec3& size, float chamfer, const glm::mat4& transform = glm::mat4(1.0f),
                              BoxUv uv = BoxUv::PerFace, float metersPerTile = 1.0f);

    /// Rectangle in the XZ plane facing +Y, centered on the origin. uv (0, 0) at the -X/-Z corner, u grows
    /// along +X up to uvTiling.x, v grows along +Z up to uvTiling.y.
    MeshBuilder& plane(const glm::vec2& sizeXZ, const glm::mat4& transform = glm::mat4(1.0f),
                       const glm::vec2& uvTiling = {1.0f, 1.0f});

    /// Rectangle in the XY plane facing +Z, centered on the origin. `uvRect` = (uLeft, vTop, uRight, vBottom):
    /// the top-left corner (-X, +Y) gets (uLeft, vTop), the bottom-right corner gets (uRight, vBottom); the
    /// default maps the whole image upright.
    MeshBuilder& quad(const glm::vec2& sizeXY, const glm::mat4& transform = glm::mat4(1.0f),
                      const glm::vec4& uvRect = {0.0f, 0.0f, 1.0f, 1.0f});

    /// UV sphere centered on the origin with poles on the Y axis. `rings` latitude bands, `segments` longitude
    /// bands; the seam column is duplicated (at -Z) and each pole has one vertex per segment, so normals are
    /// exact everywhere. u grows eastwards seen from outside (u = 0.5 faces +Z), v = 0 at the north pole (+Y).
    /// 2 * segments + (rings - 1) * (segments + 1) vertices, 6 * segments * (rings - 1) indices.
    MeshBuilder& uvSphere(float radius, int segments, int rings, const glm::mat4& transform = glm::mat4(1.0f));

    /// Cylinder centered on the origin with its axis along +Y (frustum with equal radii).
    MeshBuilder& cylinder(float radius, float height, int segments, bool capped,
                          const glm::mat4& transform = glm::mat4(1.0f));

    /// Truncated cone centered on the origin, axis +Y, bottom at y = -height / 2. Side uv: u around the axis as for
    /// uvSphere, v = 0 at the top rim and 1 at the bottom rim; caps map the image over their disc seen from outside
    /// (top cap: image top towards -Z, bottom cap: towards +Z). A zero radius makes an apex (one vertex per segment,
    /// no degenerate triangles). `doubleSided` duplicates every surface with inverted normals and winding, so the
    /// shape renders from inside and outside with back-face culling.
    MeshBuilder& frustum(float bottomRadius, float topRadius, float height, int segments, bool capped, bool doubleSided,
                         const glm::mat4& transform = glm::mat4(1.0f));

    /// Appends an existing mesh (its triangles must reference valid vertices; incomplete or out-of-range
    /// triangles are dropped). A mesh with a non-finite position or uv on any vertex is skipped as a whole;
    /// non-finite normals and tangents are replaced by valid ones.
    MeshBuilder& append(const MeshData& mesh, const glm::mat4& transform = glm::mat4(1.0f));

    /// Moves the accumulated geometry into a mesh named `name`, computes its bounds and recomputes tangents from
    /// the final uvs (computeTangents). Vertices whose triangles all have degenerate uvs keep the tangent carried
    /// through the primitive transforms instead of an arbitrary one.
    [[nodiscard]] MeshData build(std::string name) &&;

    /// True while no triangle has been appended.
    [[nodiscard]] bool empty() const noexcept { return m_indices.empty(); }

private:
    std::vector<Vertex> m_vertices;
    std::vector<std::uint32_t> m_indices;
};

} // namespace rv::geometry
