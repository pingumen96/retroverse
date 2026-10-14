#pragma once

// Geometric invariants shared by the rv_geometry tests (winding, normals, tangent frames, ray casts, z-fighting).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include <rv/core/math.hpp>
#include <rv/geometry/mesh_data.hpp>

namespace rv::test {

struct TriangleRef {
    const geometry::Vertex& v0;
    const geometry::Vertex& v1;
    const geometry::Vertex& v2;

    /// (p1 - p0) x (p2 - p0): points to the side from which the triangle is counter-clockwise.
    [[nodiscard]] glm::vec3 cross() const { return glm::cross(v1.position - v0.position, v2.position - v0.position); }
    [[nodiscard]] glm::vec3 centroid() const { return (v0.position + v1.position + v2.position) / 3.0f; }
};

[[nodiscard]] inline TriangleRef triangleAt(const geometry::MeshData& mesh, std::size_t t) {
    return {mesh.vertices[mesh.indices[3 * t]], mesh.vertices[mesh.indices[3 * t + 1]],
            mesh.vertices[mesh.indices[3 * t + 2]]};
}

/// Triangles (larger than `minDoubleArea`) whose counter-clockwise geometric normal disagrees with one of
/// their vertex normals: a wrong winding or an inverted normal.
[[nodiscard]] inline std::size_t windingMismatches(const geometry::MeshData& mesh, float minDoubleArea = 1e-10f) {
    std::size_t count = 0;
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const TriangleRef tri = triangleAt(mesh, t);
        const glm::vec3 g = tri.cross();
        const float length = glm::length(g);
        if (length < minDoubleArea) {
            continue;
        }
        const glm::vec3 n = g / length;
        if (glm::dot(n, tri.v0.normal) <= 0.0f || glm::dot(n, tri.v1.normal) <= 0.0f ||
            glm::dot(n, tri.v2.normal) <= 0.0f) {
            ++count;
        }
    }
    return count;
}

/// Vertices whose normal is not unit length.
[[nodiscard]] inline std::size_t nonUnitNormals(const geometry::MeshData& mesh, float tolerance = 1e-4f) {
    std::size_t count = 0;
    for (const geometry::Vertex& v : mesh.vertices) {
        if (std::fabs(glm::length(v.normal) - 1.0f) > tolerance) {
            ++count;
        }
    }
    return count;
}

/// Vertices whose tangent is not a unit vector orthogonal to the normal with handedness +1 or -1.
[[nodiscard]] inline std::size_t badTangents(const geometry::MeshData& mesh, float tolerance = 1e-3f) {
    std::size_t count = 0;
    for (const geometry::Vertex& v : mesh.vertices) {
        const glm::vec3 t(v.tangent);
        if (std::fabs(glm::length(t) - 1.0f) > tolerance || std::fabs(glm::dot(t, v.normal)) > tolerance ||
            std::fabs(std::fabs(v.tangent.w) - 1.0f) > 0.0f) {
            ++count;
        }
    }
    return count;
}

/// Triangles with a usable uv layout where a vertex tangent frame contradicts the uv derivatives: the tangent
/// must follow dP/du and the bitangent cross(n, t) * w must follow dP/dv.
[[nodiscard]] inline std::size_t tangentFrameMismatches(const geometry::MeshData& mesh) {
    std::size_t count = 0;
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const TriangleRef tri = triangleAt(mesh, t);
        const glm::vec3 e1 = tri.v1.position - tri.v0.position;
        const glm::vec3 e2 = tri.v2.position - tri.v0.position;
        const glm::vec2 d1 = tri.v1.uv0 - tri.v0.uv0;
        const glm::vec2 d2 = tri.v2.uv0 - tri.v0.uv0;
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-9f || glm::length(glm::cross(e1, e2)) < 1e-10f) {
            continue;
        }
        const glm::vec3 dPdu = (e1 * d2.y - e2 * d1.y) / det;
        const glm::vec3 dPdv = (e2 * d1.x - e1 * d2.x) / det;
        for (const geometry::Vertex* v : {&tri.v0, &tri.v1, &tri.v2}) {
            const glm::vec3 tangent(v->tangent);
            const glm::vec3 bitangent = glm::cross(v->normal, tangent) * v->tangent.w;
            if (glm::dot(tangent, dPdu) <= 0.0f || glm::dot(bitangent, dPdv) <= 0.0f) {
                ++count;
                break;
            }
        }
    }
    return count;
}

/// Two-sided ray/triangle intersection (Moller-Trumbore); returns the ray parameter.
[[nodiscard]] inline std::optional<float> intersect(const core::Ray& ray, const glm::vec3& a, const glm::vec3& b,
                                                    const glm::vec3& c) {
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 p = glm::cross(ray.direction, e2);
    const float det = glm::dot(e1, p);
    if (std::fabs(det) < 1e-12f) {
        return std::nullopt;
    }
    const float inverse = 1.0f / det;
    const glm::vec3 s = ray.origin - a;
    const float u = glm::dot(s, p) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return std::nullopt;
    }
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(ray.direction, q) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return std::nullopt;
    }
    const float t = glm::dot(e2, q) * inverse;
    if (t < 0.0f) {
        return std::nullopt;
    }
    return t;
}

/// Triangles of `mesh` hit by `ray` within `maxDistance` (direction need not be unit; distances scale with it).
[[nodiscard]] inline std::size_t rayHits(const geometry::MeshData& mesh, const core::Ray& ray, float maxDistance) {
    std::size_t hits = 0;
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const TriangleRef tri = triangleAt(mesh, t);
        if (const auto hit = intersect(ray, tri.v0.position, tri.v1.position, tri.v2.position);
            hit && *hit <= maxDistance) {
            ++hits;
        }
    }
    return hits;
}

/// Pairs of triangles (within or across meshes) that face the same way, lie within `planeTolerance` of the same
/// plane and overlap with a positive area: they would z-fight. With `includeOpposite`, coincident triangles facing
/// opposite ways count too (they z-fight when the material disables back-face culling).
[[nodiscard]] inline std::size_t coplanarOverlaps(const std::vector<const geometry::MeshData*>& meshes,
                                                  float planeTolerance = 0.0004f, bool includeOpposite = false) {
    struct Flat {
        glm::vec3 p[3];
        glm::vec3 n;
        float d;
    };
    std::vector<Flat> flats;
    for (const geometry::MeshData* mesh : meshes) {
        for (std::size_t t = 0; t < mesh->triangleCount(); ++t) {
            const TriangleRef tri = triangleAt(*mesh, t);
            const glm::vec3 g = tri.cross();
            const float length = glm::length(g);
            if (length < 1e-10f) {
                continue;
            }
            const glm::vec3 n = g / length;
            flats.push_back({{tri.v0.position, tri.v1.position, tri.v2.position}, n, glm::dot(n, tri.v0.position)});
        }
    }
    const auto separated = [](const glm::vec2* a, const glm::vec2* b) {
        constexpr float kEpsilon = 1e-6f;
        for (const glm::vec2* shape : {a, b}) {
            for (int e = 0; e < 3; ++e) {
                const glm::vec2 edge = shape[(e + 1) % 3] - shape[e];
                const glm::vec2 axis(-edge.y, edge.x);
                const float length = glm::length(axis);
                if (length < 1e-12f) {
                    continue;
                }
                float minA = 1e30f;
                float maxA = -1e30f;
                float minB = 1e30f;
                float maxB = -1e30f;
                for (int k = 0; k < 3; ++k) {
                    const float pa = glm::dot(a[k], axis) / length;
                    const float pb = glm::dot(b[k], axis) / length;
                    minA = std::min(minA, pa);
                    maxA = std::max(maxA, pa);
                    minB = std::min(minB, pb);
                    maxB = std::max(maxB, pb);
                }
                if (maxA <= minB + kEpsilon || maxB <= minA + kEpsilon) {
                    return true;
                }
            }
        }
        return false;
    };
    std::size_t overlaps = 0;
    for (std::size_t i = 0; i < flats.size(); ++i) {
        const Flat& a = flats[i];
        const glm::vec3 helper = std::fabs(a.n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 u = glm::normalize(glm::cross(helper, a.n));
        const glm::vec3 v = glm::cross(a.n, u);
        for (std::size_t j = i + 1; j < flats.size(); ++j) {
            const Flat& b = flats[j];
            const float facing = glm::dot(a.n, b.n);
            const bool same = facing > 0.9999f && std::fabs(a.d - b.d) <= planeTolerance;
            const bool opposite = includeOpposite && facing < -0.9999f && std::fabs(a.d + b.d) <= planeTolerance;
            if (!same && !opposite) {
                continue;
            }
            glm::vec2 pa[3];
            glm::vec2 pb[3];
            for (int k = 0; k < 3; ++k) {
                pa[k] = glm::vec2(glm::dot(a.p[k], u), glm::dot(a.p[k], v));
                pb[k] = glm::vec2(glm::dot(b.p[k], u), glm::dot(b.p[k], v));
            }
            if (!separated(pa, pb)) {
                ++overlaps;
            }
        }
    }
    return overlaps;
}

} // namespace rv::test
