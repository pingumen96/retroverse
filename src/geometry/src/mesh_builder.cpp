#include <rv/geometry/mesh_builder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include <rv/core/math.hpp>

#include "mesh_detail.hpp"

namespace rv::geometry {
namespace {

using detail::addTriangle;
using detail::addVertex;

constexpr int kMinSegments = 3;
constexpr int kMaxSegments = 1024;
constexpr int kMinRings = 2;
constexpr int kMaxRings = 512;
/// Linear parts with a smaller |determinant| are treated as singular (uniform scale below about 1e-10).
constexpr float kMinDeterminant = 1e-30f;
/// chamferedBox emits a plain box below this chamfer (meters).
constexpr float kMinChamfer = 1e-5f;
/// Largest chamfer as a fraction of the smallest box dimension.
constexpr float kMaxChamferFraction = 0.45f;

[[nodiscard]] bool isFinite(const glm::vec2& v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y);
}

/// Affine placement of a primitive: positions by the full matrix, normals by the inverse-transpose of the upper
/// 3x3, tangents by the upper 3x3; mirroring flips the tangent handedness (the winding is fixed by appendMesh).
struct Affine {
    glm::mat3 linear{1.0f};
    glm::vec3 translation{0.0f};
    /// Cofactor matrix of `linear` times sign(det): the inverse-transpose up to a positive scale, computed
    /// without dividing by the determinant.
    glm::mat3 normalMatrix{1.0f};
    float handedness = 1.0f;

    [[nodiscard]] bool mirrored() const noexcept { return handedness < 0.0f; }

    [[nodiscard]] glm::vec3 point(const glm::vec3& p) const noexcept { return linear * p + translation; }

    [[nodiscard]] Vertex vertex(const Vertex& v) const noexcept {
        Vertex out;
        out.position = point(v.position);
        out.normal = detail::normalizeOr(normalMatrix * v.normal, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 tangent = linear * glm::vec3(v.tangent);
        const float sign = v.tangent.w < 0.0f ? -1.0f : 1.0f;
        out.tangent = glm::vec4(detail::orthonormalize(tangent, out.normal, tangent), sign * handedness);
        out.uv0 = v.uv0;
        return out;
    }
};

[[nodiscard]] std::optional<Affine> makeAffine(const glm::mat4& m) noexcept {
    if (!core::isFinite(m)) {
        return std::nullopt;
    }
    Affine affine;
    affine.linear = glm::mat3(m);
    affine.translation = glm::vec3(m[3]);
    const float determinant = glm::determinant(affine.linear);
    if (!std::isfinite(determinant) || std::fabs(determinant) < kMinDeterminant) {
        return std::nullopt;
    }
    affine.handedness = determinant < 0.0f ? -1.0f : 1.0f;
    const glm::mat3 cofactor(glm::cross(affine.linear[1], affine.linear[2]),
                             glm::cross(affine.linear[2], affine.linear[0]),
                             glm::cross(affine.linear[0], affine.linear[1]));
    affine.normalMatrix = cofactor * affine.handedness;
    return affine;
}

/// Appends the complete, in-range triangles of `source` (and its vertices) placed by `affine`. Mirroring
/// transforms swap two indices per triangle so front faces stay counter-clockwise. Nothing is appended when a
/// placed position or a uv is not finite (non-finite source data or a placement beyond the float range).
void appendMesh(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, const MeshData& source,
                const Affine& affine) {
    const std::size_t sourceVertexCount = source.vertices.size();
    const std::size_t base = vertices.size();
    if (sourceVertexCount == 0 || base + sourceVertexCount > std::numeric_limits<std::uint32_t>::max()) {
        return;
    }
    const auto offset = static_cast<std::uint32_t>(base);
    const std::size_t firstIndex = indices.size();
    const std::size_t completeIndexCount = source.indices.size() - source.indices.size() % 3;
    indices.reserve(firstIndex + completeIndexCount);
    for (std::size_t i = 0; i < completeIndexCount; i += 3) {
        const std::uint32_t a = source.indices[i];
        std::uint32_t b = source.indices[i + 1];
        std::uint32_t c = source.indices[i + 2];
        if (a >= sourceVertexCount || b >= sourceVertexCount || c >= sourceVertexCount) {
            continue;
        }
        if (affine.mirrored()) {
            std::swap(b, c);
        }
        indices.push_back(offset + a);
        indices.push_back(offset + b);
        indices.push_back(offset + c);
    }
    if (indices.size() == firstIndex) {
        return;
    }
    vertices.reserve(base + sourceVertexCount);
    for (const Vertex& v : source.vertices) {
        const Vertex placed = affine.vertex(v);
        if (!core::isFinite(placed.position) || !isFinite(placed.uv0)) {
            vertices.resize(base);
            indices.resize(firstIndex);
            return;
        }
        vertices.push_back(placed);
    }
}

/// Box face frame; normal = cross(right, up). "Up" is the image top seen from outside, so uv (0, 0) sits at
/// center - right + up.
struct FaceFrame {
    glm::vec3 normal;
    glm::vec3 right;
    glm::vec3 up;
    FaceMask bit;
};

/// The six box faces, indexed by axis * 2 + (negative ? 1 : 0).
[[nodiscard]] const std::array<FaceFrame, 6>& boxFaces() {
    static const std::array<FaceFrame, 6> faces{{
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}, FaceMask::PosX},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, FaceMask::NegX},
        {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, FaceMask::PosY},
        {{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, FaceMask::NegY},
        {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, FaceMask::PosZ},
        {{0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, FaceMask::NegZ},
    }};
    return faces;
}

[[nodiscard]] const FaceFrame& boxFace(int axis, float sign) {
    return boxFaces()[static_cast<std::size_t>(axis * 2 + (sign < 0.0f ? 1 : 0))];
}

/// Box face whose uv projection a bevel facing `outward` reuses: the dominant axis, Z before X before Y on ties.
[[nodiscard]] const FaceFrame& projectionFace(const glm::vec3& outward) {
    const glm::vec3 a = glm::abs(outward);
    int axis = 1;
    if (a.z >= a.x && a.z >= a.y) {
        axis = 2;
    } else if (a.x >= a.y) {
        axis = 0;
    }
    return boxFace(axis, outward[axis]);
}

/// BoxUv::PerFace coordinates of `p` projected on the full face of a box with half extents `half`.
[[nodiscard]] glm::vec2 projectedUv(const glm::vec3& p, const glm::vec3& half, const FaceFrame& face) {
    const float halfWidth = glm::dot(glm::abs(face.right), half);
    const float halfHeight = glm::dot(glm::abs(face.up), half);
    return {0.5f + glm::dot(p, face.right) / (2.0f * halfWidth), 0.5f - glm::dot(p, face.up) / (2.0f * halfHeight)};
}

/// Uv of the box-local point `p` mapped through `face` of a box with half extents `half` placed by `affine`, in the
/// layout `uv` (BoxUv); meter layouts divide by `tile`.
[[nodiscard]] glm::vec2 faceUv(const glm::vec3& p, const glm::vec3& half, const FaceFrame& face, const Affine& affine,
                               BoxUv uv, float tile) {
    if (uv == BoxUv::PerFace) {
        return projectedUv(p, half, face);
    }
    // Face-plane coordinates in builder space: right and down axes of the transformed face.
    const glm::vec3 right = detail::normalizeOr(affine.linear * face.right, face.right);
    const glm::vec3 down = detail::normalizeOr(affine.linear * -face.up, -face.up);
    const glm::vec3 placed = affine.point(p);
    glm::vec2 meters(glm::dot(placed, right), glm::dot(placed, down));
    if (uv == BoxUv::GrainMeters) {
        const float width = glm::length(affine.linear * face.right) * glm::dot(glm::abs(face.right), half);
        const float height = glm::length(affine.linear * face.up) * glm::dot(glm::abs(face.up), half);
        if (height > width) {
            meters = glm::vec2(meters.y, -meters.x); // u down the face, v towards its left: a quarter turn
        }
    }
    return meters / tile;
}

/// Rectangle corners in the order top-left, bottom-left, bottom-right, top-right.
using Corners = std::array<glm::vec3, 4>;
using CornerUvs = std::array<glm::vec2, 4>;

[[nodiscard]] Corners rectCorners(const glm::vec3& center, const glm::vec3& right, const glm::vec3& up, float halfWidth,
                                  float halfHeight) {
    const glm::vec3 r = right * halfWidth;
    const glm::vec3 u = up * halfHeight;
    return {center - r + u, center - r - u, center + r - u, center + r + u};
}

/// Two triangles, counter-clockwise seen from `normal` = cross(right, up).
void addRect(MeshData& mesh, const Corners& corners, const CornerUvs& uvs, const glm::vec3& normal,
             const glm::vec3& right) {
    const glm::vec4 tangent = detail::triangleTangent(normal, corners[1] - corners[0], corners[2] - corners[0],
                                                      uvs[1] - uvs[0], uvs[2] - uvs[0], right);
    const std::uint32_t first = addVertex(mesh, corners[0], normal, tangent, uvs[0]);
    for (std::size_t i = 1; i < 4; ++i) {
        addVertex(mesh, corners[i], normal, tangent, uvs[i]);
    }
    addTriangle(mesh, first, first + 1, first + 2);
    addTriangle(mesh, first, first + 2, first + 3);
}

/// Longitudes shared by spheres and frusta: phi_i = 2 pi i / n measured from -Z towards -X, so u = i / n grows
/// eastwards seen from outside and u = 0.5 faces +Z. Column n repeats column 0 bit for bit (watertight seam).
class Azimuth {
public:
    explicit Azimuth(int segments) : m_segments(segments) {
        const auto count = static_cast<std::size_t>(segments);
        m_columns.resize(count + 1);
        m_midpoints.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            const float phi = core::kTwoPi * static_cast<float>(i) / static_cast<float>(segments);
            const float midPhi = core::kTwoPi * (static_cast<float>(i) + 0.5f) / static_cast<float>(segments);
            m_columns[i] = {std::sin(phi), std::cos(phi)};
            m_midpoints[i] = {std::sin(midPhi), std::cos(midPhi)};
        }
        m_columns[count] = m_columns[0];
    }

    [[nodiscard]] int segments() const noexcept { return m_segments; }
    /// Unit radial direction of column i (0..n) or of the middle of segment i (0..n-1).
    [[nodiscard]] glm::vec3 radial(int i) const noexcept { return radialOf(m_columns[index(i)]); }
    [[nodiscard]] glm::vec3 midRadial(int i) const noexcept { return radialOf(m_midpoints[index(i)]); }
    /// Unit direction of growing u (eastwards) at column i or in the middle of segment i.
    [[nodiscard]] glm::vec3 tangent(int i) const noexcept { return tangentOf(m_columns[index(i)]); }
    [[nodiscard]] glm::vec3 midTangent(int i) const noexcept { return tangentOf(m_midpoints[index(i)]); }
    [[nodiscard]] float u(int i) const noexcept { return static_cast<float>(i) / static_cast<float>(m_segments); }
    [[nodiscard]] float midU(int i) const noexcept {
        return (static_cast<float>(i) + 0.5f) / static_cast<float>(m_segments);
    }

private:
    [[nodiscard]] static std::size_t index(int i) noexcept { return static_cast<std::size_t>(i); }
    [[nodiscard]] static glm::vec3 radialOf(const glm::vec2& sc) noexcept { return {-sc.x, 0.0f, -sc.y}; }
    [[nodiscard]] static glm::vec3 tangentOf(const glm::vec2& sc) noexcept { return {-sc.y, 0.0f, sc.x}; }

    int m_segments;
    std::vector<glm::vec2> m_columns;   // (sin phi, cos phi)
    std::vector<glm::vec2> m_midpoints; // (sin phi, cos phi) at segment midpoints
};

/// Appends every triangle again facing the other way (inverted normals, reversed winding, flipped handedness).
void addBackFaces(MeshData& mesh) {
    const std::size_t vertexCount = mesh.vertices.size();
    const std::size_t indexCount = mesh.indices.size();
    const auto offset = static_cast<std::uint32_t>(vertexCount);
    mesh.vertices.reserve(2 * vertexCount);
    mesh.indices.reserve(2 * indexCount);
    for (std::size_t v = 0; v < vertexCount; ++v) {
        Vertex inner = mesh.vertices[v];
        inner.normal = -inner.normal;
        inner.tangent.w = -inner.tangent.w;
        mesh.vertices.push_back(inner);
    }
    for (std::size_t i = 0; i < indexCount; i += 3) {
        const std::uint32_t a = mesh.indices[i] + offset;
        const std::uint32_t b = mesh.indices[i + 1] + offset;
        const std::uint32_t c = mesh.indices[i + 2] + offset;
        addTriangle(mesh, a, c, b);
    }
}

/// Recomputes tangents from uvs; vertices without any uv-defined triangle keep their carried tangent.
void finalizeTangents(MeshData& mesh) {
    const std::size_t vertexCount = mesh.vertices.size();
    std::vector<std::uint8_t> uvDefined(vertexCount, 0);
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::uint32_t i0 = mesh.indices[i];
        const std::uint32_t i1 = mesh.indices[i + 1];
        const std::uint32_t i2 = mesh.indices[i + 2];
        const glm::vec2 d1 = mesh.vertices[i1].uv0 - mesh.vertices[i0].uv0;
        const glm::vec2 d2 = mesh.vertices[i2].uv0 - mesh.vertices[i0].uv0;
        if (std::fabs(d1.x * d2.y - d2.x * d1.y) >= detail::kDegenerateUvArea) {
            uvDefined[i0] = 1;
            uvDefined[i1] = 1;
            uvDefined[i2] = 1;
        }
    }
    std::vector<glm::vec4> carried;
    carried.reserve(vertexCount);
    for (const Vertex& v : mesh.vertices) {
        carried.push_back(v.tangent);
    }
    computeTangents(mesh);
    for (std::size_t v = 0; v < vertexCount; ++v) {
        if (uvDefined[v] == 0) {
            Vertex& vertex = mesh.vertices[v];
            const glm::vec3 tangent(carried[v]);
            vertex.tangent = glm::vec4(detail::orthonormalize(tangent, vertex.normal, tangent), carried[v].w);
        }
    }
}

} // namespace

MeshBuilder& MeshBuilder::box(const glm::vec3& size, const glm::mat4& transform, BoxUv uv, float metersPerTile,
                              FaceMask faces) {
    const std::optional<Affine> affine = makeAffine(transform);
    if (!affine || !core::isFinite(size) || !std::isfinite(metersPerTile)) {
        return *this;
    }
    const glm::vec3 half = glm::abs(size) * 0.5f;
    const float tile = metersPerTile > 0.0f ? metersPerTile : 1.0f;
    MeshData local;
    for (const FaceFrame& face : boxFaces()) {
        if (!hasAny(faces, face.bit)) {
            continue;
        }
        const Corners corners = rectCorners(face.normal * glm::dot(glm::abs(face.normal), half), face.right, face.up,
                                            glm::dot(glm::abs(face.right), half), glm::dot(glm::abs(face.up), half));
        CornerUvs uvs{glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, 1.0f), glm::vec2(1.0f, 1.0f), glm::vec2(1.0f, 0.0f)};
        if (uv != BoxUv::PerFace) {
            for (std::size_t i = 0; i < corners.size(); ++i) {
                uvs[i] = faceUv(corners[i], half, face, *affine, uv, tile);
            }
        }
        addRect(local, corners, uvs, face.normal, face.right);
    }
    appendMesh(m_vertices, m_indices, local, *affine);
    return *this;
}

MeshBuilder& MeshBuilder::chamferedBox(const glm::vec3& size, float chamfer, const glm::mat4& transform, BoxUv uv,
                                       float metersPerTile, FaceMask bevelled) {
    const std::optional<Affine> affine = makeAffine(transform);
    if (!affine || !core::isFinite(size) || !std::isfinite(chamfer) || !std::isfinite(metersPerTile)) {
        return *this;
    }
    const glm::vec3 half = glm::abs(size) * 0.5f;
    const float tile = metersPerTile > 0.0f ? metersPerTile : 1.0f;
    const float smallest = 2.0f * std::min({half.x, half.y, half.z});
    const float c = std::min(std::fabs(chamfer), kMaxChamferFraction * smallest);
    if (c < kMinChamfer || bevelled == FaceMask::None) {
        return box(size, transform, uv, tile);
    }

    // The edge between the face perpendicular to `axisA` on the side `signA` and the one perpendicular to `axisB` on
    // the side `signB` is bevelled when either face is in `bevelled`. So the corner where three faces meet has all
    // three edges bevelled (a corner triangle), two (the two strips meet in a mitre) or none.
    const auto bevelledEdge = [&](int axisA, float signA, int axisB, float signB) {
        return hasAny(bevelled, boxFace(axisA, signA).bit | boxFace(axisB, signB).bit);
    };
    // Vertex of the bevelled box lying on the face perpendicular to `axis`, in the octant given by `signs`: inset by
    // the chamfer along each other axis whose edge with that face is bevelled.
    const auto corner = [&](int axis, const glm::vec3& signs) {
        glm::vec3 p = signs * half;
        for (int other = 0; other < 3; ++other) {
            if (other != axis && bevelledEdge(axis, signs[axis], other, signs[other])) {
                p[other] = signs[other] * (half[other] - c);
            }
        }
        return p;
    };
    MeshData local;
    std::array<glm::vec3, 4> points{};
    std::array<glm::vec2, 4> uvs{};
    const auto emit = [&](std::size_t count, const glm::vec3& outward) {
        const FaceFrame& face = projectionFace(outward);
        for (std::size_t i = 0; i < count; ++i) {
            uvs[i] = faceUv(points[i], half, face, *affine, uv, tile);
        }
        detail::addFlatPolygon(local, std::span<const glm::vec3>(points.data(), count),
                               std::span<const glm::vec2>(uvs.data(), count), outward);
    };

    // Six faces, shrunk by the chamfer along their bevelled edges.
    constexpr std::array<std::array<float, 2>, 4> kSquare{{{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};
    for (int axis = 0; axis < 3; ++axis) {
        const int second = (axis + 1) % 3;
        const int third = (axis + 2) % 3;
        for (const float sign : {1.0f, -1.0f}) {
            for (std::size_t i = 0; i < kSquare.size(); ++i) {
                glm::vec3 signs(0.0f);
                signs[axis] = sign;
                signs[second] = kSquare[i][0];
                signs[third] = kSquare[i][1];
                points[i] = corner(axis, signs);
            }
            glm::vec3 outward(0.0f);
            outward[axis] = sign;
            emit(4, outward);
        }
    }
    // A 45-degree strip per bevelled edge (the edge runs along `edge`).
    for (int edge = 0; edge < 3; ++edge) {
        const int second = (edge + 1) % 3;
        const int third = (edge + 2) % 3;
        for (const float secondSign : {1.0f, -1.0f}) {
            for (const float thirdSign : {1.0f, -1.0f}) {
                if (!bevelledEdge(second, secondSign, third, thirdSign)) {
                    continue;
                }
                glm::vec3 outward(0.0f);
                outward[second] = secondSign;
                outward[third] = thirdSign;
                glm::vec3 start = outward;
                start[edge] = -1.0f;
                glm::vec3 end = outward;
                end[edge] = 1.0f;
                points = {corner(second, start), corner(second, end), corner(third, end), corner(third, start)};
                emit(4, outward);
            }
        }
    }
    // A triangle per corner whose three edges are bevelled.
    for (const float sx : {1.0f, -1.0f}) {
        for (const float sy : {1.0f, -1.0f}) {
            for (const float sz : {1.0f, -1.0f}) {
                if (!bevelledEdge(0, sx, 1, sy) || !bevelledEdge(0, sx, 2, sz) || !bevelledEdge(1, sy, 2, sz)) {
                    continue;
                }
                const glm::vec3 signs(sx, sy, sz);
                points[0] = corner(0, signs);
                points[1] = corner(1, signs);
                points[2] = corner(2, signs);
                emit(3, signs);
            }
        }
    }
    appendMesh(m_vertices, m_indices, local, *affine);
    return *this;
}

MeshBuilder& MeshBuilder::plane(const glm::vec2& sizeXZ, const glm::mat4& transform, const glm::vec2& uvTiling) {
    const std::optional<Affine> affine = makeAffine(transform);
    if (!affine || !isFinite(sizeXZ) || !isFinite(uvTiling)) {
        return *this;
    }
    const glm::vec2 half = glm::abs(sizeXZ) * 0.5f;
    const glm::vec3 right(1.0f, 0.0f, 0.0f);
    const glm::vec3 up(0.0f, 0.0f, -1.0f); // top view: the image top points to -Z
    MeshData local;
    const Corners corners = rectCorners(glm::vec3(0.0f), right, up, half.x, half.y);
    const CornerUvs uvs{glm::vec2(0.0f, 0.0f), glm::vec2(0.0f, uvTiling.y), uvTiling, glm::vec2(uvTiling.x, 0.0f)};
    addRect(local, corners, uvs, glm::vec3(0.0f, 1.0f, 0.0f), right);
    appendMesh(m_vertices, m_indices, local, *affine);
    return *this;
}

MeshBuilder& MeshBuilder::quad(const glm::vec2& sizeXY, const glm::mat4& transform, const glm::vec4& uvRect) {
    const std::optional<Affine> affine = makeAffine(transform);
    if (!affine || !isFinite(sizeXY) || !core::isFinite(uvRect)) {
        return *this;
    }
    const glm::vec2 half = glm::abs(sizeXY) * 0.5f;
    const glm::vec3 right(1.0f, 0.0f, 0.0f);
    MeshData local;
    const Corners corners = rectCorners(glm::vec3(0.0f), right, glm::vec3(0.0f, 1.0f, 0.0f), half.x, half.y);
    const CornerUvs uvs{glm::vec2(uvRect.x, uvRect.y), glm::vec2(uvRect.x, uvRect.w), glm::vec2(uvRect.z, uvRect.w),
                        glm::vec2(uvRect.z, uvRect.y)};
    addRect(local, corners, uvs, glm::vec3(0.0f, 0.0f, 1.0f), right);
    appendMesh(m_vertices, m_indices, local, *affine);
    return *this;
}

MeshBuilder& MeshBuilder::uvSphere(float radius, int segments, int rings, const glm::mat4& transform) {
    const std::optional<Affine> affine = makeAffine(transform);
    if (!affine || !std::isfinite(radius)) {
        return *this;
    }
    const float r = std::fabs(radius);
    const Azimuth azimuth(std::clamp(segments, kMinSegments, kMaxSegments));
    const int segmentCount = azimuth.segments();
    const int ringCount = std::clamp(rings, kMinRings, kMaxRings);
    const auto columns = static_cast<std::uint32_t>(segmentCount + 1);

    MeshData local;
    const auto addPoles = [&](float y, float v) {
        for (int i = 0; i < segmentCount; ++i) {
            addVertex(local, glm::vec3(0.0f, y * r, 0.0f), glm::vec3(0.0f, y, 0.0f),
                      glm::vec4(azimuth.midTangent(i), 1.0f), glm::vec2(azimuth.midU(i), v));
        }
    };
    addPoles(1.0f, 0.0f);
    const auto firstRing = static_cast<std::uint32_t>(local.vertices.size());
    for (int ring = 1; ring < ringCount; ++ring) {
        const float v = static_cast<float>(ring) / static_cast<float>(ringCount);
        const float theta = core::kPi * v;
        const float sinTheta = std::sin(theta);
        const float cosTheta = std::cos(theta);
        for (int i = 0; i <= segmentCount; ++i) {
            const glm::vec3 radial = azimuth.radial(i);
            const glm::vec3 normal(radial.x * sinTheta, cosTheta, radial.z * sinTheta);
            addVertex(local, normal * r, normal, glm::vec4(azimuth.tangent(i), 1.0f), glm::vec2(azimuth.u(i), v));
        }
    }
    const auto southPoles = static_cast<std::uint32_t>(local.vertices.size());
    addPoles(-1.0f, 1.0f);

    // Vertex of ring `ring` (1..rings-1) at column `column` (0..segments).
    const auto at = [&](int ring, int column) {
        return firstRing + static_cast<std::uint32_t>(ring - 1) * columns + static_cast<std::uint32_t>(column);
    };
    for (int i = 0; i < segmentCount; ++i) {
        addTriangle(local, static_cast<std::uint32_t>(i), at(1, i), at(1, i + 1));
    }
    for (int ring = 1; ring + 1 < ringCount; ++ring) {
        for (int i = 0; i < segmentCount; ++i) {
            const std::uint32_t topLeft = at(ring, i);
            const std::uint32_t topRight = at(ring, i + 1);
            const std::uint32_t bottomLeft = at(ring + 1, i);
            const std::uint32_t bottomRight = at(ring + 1, i + 1);
            addTriangle(local, topLeft, bottomLeft, bottomRight);
            addTriangle(local, topLeft, bottomRight, topRight);
        }
    }
    for (int i = 0; i < segmentCount; ++i) {
        addTriangle(local, at(ringCount - 1, i), southPoles + static_cast<std::uint32_t>(i), at(ringCount - 1, i + 1));
    }
    appendMesh(m_vertices, m_indices, local, *affine);
    return *this;
}

MeshBuilder& MeshBuilder::torus(float majorRadius, float minorRadius, int segments, int sides,
                                const glm::mat4& transform) {
    const std::optional<Affine> affine = makeAffine(transform);
    if (!affine || !std::isfinite(majorRadius) || !std::isfinite(minorRadius)) {
        return *this;
    }
    const float major = std::fabs(majorRadius);
    const float minor = std::fabs(minorRadius);
    if (!(major > 0.0f) || !(minor > 0.0f) || minor >= major) {
        return *this;
    }
    const Azimuth azimuth(std::clamp(segments, kMinSegments, kMaxSegments));
    const int segmentCount = azimuth.segments();
    const int sideCount = std::clamp(sides, kMinSegments, kMaxSegments);
    const auto columns = static_cast<std::uint32_t>(sideCount + 1);

    MeshData local;
    for (int i = 0; i <= segmentCount; ++i) {
        const glm::vec3 radial = azimuth.radial(i);
        const glm::vec4 tangent(azimuth.tangent(i), 1.0f);
        for (int j = 0; j <= sideCount; ++j) {
            const float v = static_cast<float>(j) / static_cast<float>(sideCount);
            const float theta = core::kTwoPi * v;
            const glm::vec3 normal = radial * std::cos(theta) + glm::vec3(0.0f, std::sin(theta), 0.0f);
            addVertex(local, radial * major + normal * minor, normal, tangent, glm::vec2(azimuth.u(i), v));
        }
    }
    // Along u (the tangent) then around the tube: their cross product is the outward normal.
    for (int i = 0; i < segmentCount; ++i) {
        for (int j = 0; j < sideCount; ++j) {
            const std::uint32_t a = static_cast<std::uint32_t>(i) * columns + static_cast<std::uint32_t>(j);
            const std::uint32_t b = a + columns;
            addTriangle(local, a, b, b + 1);
            addTriangle(local, a, b + 1, a + 1);
        }
    }
    appendMesh(m_vertices, m_indices, local, *affine);
    return *this;
}

MeshBuilder& MeshBuilder::cylinder(float radius, float height, int segments, bool capped, const glm::mat4& transform) {
    return frustum(radius, radius, height, segments, capped, false, transform);
}

MeshBuilder& MeshBuilder::frustum(float bottomRadius, float topRadius, float height, int segments, bool capped,
                                  bool doubleSided, const glm::mat4& transform) {
    const std::optional<Affine> affine = makeAffine(transform);
    if (!affine || !std::isfinite(bottomRadius) || !std::isfinite(topRadius) || !std::isfinite(height)) {
        return *this;
    }
    const float bottom = std::fabs(bottomRadius);
    const float top = std::fabs(topRadius);
    const float h = std::fabs(height);
    if (!(bottom > 0.0f) && !(top > 0.0f)) {
        return *this;
    }
    const Azimuth azimuth(std::clamp(segments, kMinSegments, kMaxSegments));
    const int segmentCount = azimuth.segments();
    const float yTop = 0.5f * h;
    const float yBottom = -0.5f * h;

    MeshData local;
    // Side: the outward normal leans by the slope, (radial * h, rb - rt) normalized.
    const float slopeLength = std::sqrt(h * h + (bottom - top) * (bottom - top));
    if (slopeLength > 1e-12f) {
        const float radialPart = h / slopeLength;
        const float verticalPart = (bottom - top) / slopeLength;
        const auto sideNormal = [&](const glm::vec3& radial) {
            return glm::vec3(radial.x * radialPart, verticalPart, radial.z * radialPart);
        };
        // A rim with a positive radius has segments + 1 columns; an apex has one vertex per segment.
        const auto addRim = [&](float radius, float y, float v) {
            const auto first = static_cast<std::uint32_t>(local.vertices.size());
            if (radius > 0.0f) {
                for (int i = 0; i <= segmentCount; ++i) {
                    const glm::vec3 radial = azimuth.radial(i);
                    addVertex(local, radial * radius + glm::vec3(0.0f, y, 0.0f), sideNormal(radial),
                              glm::vec4(azimuth.tangent(i), 1.0f), glm::vec2(azimuth.u(i), v));
                }
            } else {
                for (int i = 0; i < segmentCount; ++i) {
                    addVertex(local, glm::vec3(0.0f, y, 0.0f), sideNormal(azimuth.midRadial(i)),
                              glm::vec4(azimuth.midTangent(i), 1.0f), glm::vec2(azimuth.midU(i), v));
                }
            }
            return first;
        };
        const std::uint32_t topRim = addRim(top, yTop, 0.0f);
        const std::uint32_t bottomRim = addRim(bottom, yBottom, 1.0f);
        for (int s = 0; s < segmentCount; ++s) {
            const auto i = static_cast<std::uint32_t>(s);
            if (!(top > 0.0f)) {
                addTriangle(local, topRim + i, bottomRim + i, bottomRim + i + 1);
            } else if (!(bottom > 0.0f)) {
                addTriangle(local, topRim + i, bottomRim + i, topRim + i + 1);
            } else {
                addTriangle(local, topRim + i, bottomRim + i, bottomRim + i + 1);
                addTriangle(local, topRim + i, bottomRim + i + 1, topRim + i + 1);
            }
        }
    }
    if (capped) {
        // Planar caps: top view for the top cap (image top = -Z), bottom view for the bottom cap (image top = +Z).
        const auto addCap = [&](float radius, float y, float facing) {
            if (!(radius > 0.0f)) {
                return;
            }
            const glm::vec3 normal(0.0f, facing, 0.0f);
            const glm::vec4 tangent(1.0f, 0.0f, 0.0f, 1.0f);
            const std::uint32_t center =
                addVertex(local, glm::vec3(0.0f, y, 0.0f), normal, tangent, glm::vec2(0.5f, 0.5f));
            for (int i = 0; i < segmentCount; ++i) {
                const glm::vec3 radial = azimuth.radial(i);
                addVertex(local, radial * radius + glm::vec3(0.0f, y, 0.0f), normal, tangent,
                          glm::vec2(0.5f + 0.5f * radial.x, 0.5f + 0.5f * facing * radial.z));
            }
            const auto count = static_cast<std::uint32_t>(segmentCount);
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t a = center + 1 + i;
                const std::uint32_t b = center + 1 + (i + 1) % count;
                if (facing > 0.0f) {
                    addTriangle(local, center, a, b);
                } else {
                    addTriangle(local, center, b, a);
                }
            }
        };
        addCap(top, yTop, 1.0f);
        addCap(bottom, yBottom, -1.0f);
    }
    if (doubleSided) {
        addBackFaces(local);
    }
    appendMesh(m_vertices, m_indices, local, *affine);
    return *this;
}

MeshBuilder& MeshBuilder::append(const MeshData& mesh, const glm::mat4& transform) {
    const std::optional<Affine> affine = makeAffine(transform);
    if (affine) {
        appendMesh(m_vertices, m_indices, mesh, *affine);
    }
    return *this;
}

MeshData MeshBuilder::build(std::string name) && {
    MeshData mesh;
    mesh.name = std::move(name);
    mesh.vertices = std::move(m_vertices);
    mesh.indices = std::move(m_indices);
    m_vertices.clear();
    m_indices.clear();
    computeBounds(mesh);
    finalizeTangents(mesh);
    return mesh;
}

} // namespace rv::geometry
