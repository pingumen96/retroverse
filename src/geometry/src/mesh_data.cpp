#include <rv/geometry/mesh_data.hpp>

#include <cmath>
#include <cstring>
#include <string>

#include <rv/core/math.hpp>

namespace rv::geometry {

const VertexLayout& VertexLayout::standard() noexcept {
    static const VertexLayout layout = [] {
        VertexLayout l;
        l.attributes[0] = {0, 3, static_cast<std::uint32_t>(offsetof(Vertex, position))};
        l.attributes[1] = {1, 3, static_cast<std::uint32_t>(offsetof(Vertex, normal))};
        l.attributes[2] = {2, 4, static_cast<std::uint32_t>(offsetof(Vertex, tangent))};
        l.attributes[3] = {3, 2, static_cast<std::uint32_t>(offsetof(Vertex, uv0))};
        l.stride = static_cast<std::uint32_t>(sizeof(Vertex));
        return l;
    }();
    return layout;
}

void computeBounds(MeshData& mesh) noexcept {
    core::Aabb bounds;
    for (const Vertex& v : mesh.vertices) {
        bounds.expand(v.position);
    }
    mesh.bounds = bounds;
}

namespace {

glm::vec3 anyPerpendicular(const glm::vec3& n) {
    const glm::vec3 axis = std::fabs(n.x) < 0.9f ? glm::vec3(1.0f, 0.0f, 0.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::normalize(glm::cross(axis, n));
}

} // namespace

void computeTangents(MeshData& mesh) {
    const std::size_t vertexCount = mesh.vertices.size();
    std::vector<glm::vec3> tangents(vertexCount, glm::vec3(0.0f));
    std::vector<glm::vec3> bitangents(vertexCount, glm::vec3(0.0f));

    const std::size_t indexCount = mesh.indices.size() - (mesh.indices.size() % 3);
    for (std::size_t i = 0; i < indexCount; i += 3) {
        const std::uint32_t i0 = mesh.indices[i];
        const std::uint32_t i1 = mesh.indices[i + 1];
        const std::uint32_t i2 = mesh.indices[i + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
            continue;
        }
        const Vertex& v0 = mesh.vertices[i0];
        const Vertex& v1 = mesh.vertices[i1];
        const Vertex& v2 = mesh.vertices[i2];

        const glm::vec3 e1 = v1.position - v0.position;
        const glm::vec3 e2 = v2.position - v0.position;
        const glm::vec2 d1 = v1.uv0 - v0.uv0;
        const glm::vec2 d2 = v2.uv0 - v0.uv0;
        const float determinant = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(determinant) < 1e-12f) {
            continue;
        }
        const float r = 1.0f / determinant;
        const glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        const glm::vec3 b = (e2 * d1.x - e1 * d2.x) * r;
        for (const std::uint32_t index : {i0, i1, i2}) {
            tangents[index] += t;
            bitangents[index] += b;
        }
    }

    for (std::size_t v = 0; v < vertexCount; ++v) {
        Vertex& vertex = mesh.vertices[v];
        const float normalLength = glm::length(vertex.normal);
        const glm::vec3 n = normalLength > 1e-12f ? vertex.normal / normalLength : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 t = tangents[v] - n * glm::dot(n, tangents[v]);
        if (glm::dot(t, t) < 1e-20f) {
            t = anyPerpendicular(n);
        } else {
            t = glm::normalize(t);
        }
        // glTF tangent space: the bitangent cross(n, t) * w follows -dP/dv (towards the top of the image), the
        // "+Y up" direction of normal maps.
        const float handedness = glm::dot(glm::cross(n, t), bitangents[v]) > 0.0f ? -1.0f : 1.0f;
        vertex.tangent = glm::vec4(t, handedness);
    }
}

glm::vec2 uvMetersPerUnit(const MeshData& mesh) noexcept {
    const std::size_t vertexCount = mesh.vertices.size();
    const std::size_t indexCount = mesh.indices.size() - (mesh.indices.size() % 3);
    glm::dvec2 weightedLength(0.0);
    double totalArea = 0.0;
    for (std::size_t i = 0; i < indexCount; i += 3) {
        const std::uint32_t i0 = mesh.indices[i];
        const std::uint32_t i1 = mesh.indices[i + 1];
        const std::uint32_t i2 = mesh.indices[i + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
            continue;
        }
        const Vertex& v0 = mesh.vertices[i0];
        const Vertex& v1 = mesh.vertices[i1];
        const Vertex& v2 = mesh.vertices[i2];
        const glm::dvec3 e1 = glm::dvec3(v1.position) - glm::dvec3(v0.position);
        const glm::dvec3 e2 = glm::dvec3(v2.position) - glm::dvec3(v0.position);
        const glm::dvec2 d1 = glm::dvec2(v1.uv0) - glm::dvec2(v0.uv0);
        const glm::dvec2 d2 = glm::dvec2(v2.uv0) - glm::dvec2(v0.uv0);
        const double determinant = d1.x * d2.y - d2.x * d1.y;
        const double area = 0.5 * glm::length(glm::cross(e1, e2));
        if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-12 || !std::isfinite(area) || area <= 0.0) {
            continue;
        }
        // Partial derivatives of the position with respect to u and v (as for tangents, not normalized).
        const glm::dvec3 dPdu = (e1 * d2.y - e2 * d1.y) / determinant;
        const glm::dvec3 dPdv = (e2 * d1.x - e1 * d2.x) / determinant;
        const glm::dvec2 lengths(glm::length(dPdu), glm::length(dPdv));
        if (!std::isfinite(lengths.x) || !std::isfinite(lengths.y)) {
            continue;
        }
        weightedLength += lengths * area;
        totalArea += area;
    }
    if (totalArea <= 0.0) {
        return glm::vec2(0.0f);
    }
    return glm::vec2(weightedLength / totalArea);
}

Status validateMesh(const MeshData& mesh) {
    const std::string context = mesh.name.empty() ? std::string("<unnamed mesh>") : mesh.name;
    if (mesh.indices.size() % 3 != 0) {
        return makeError(ErrorCode::ValidationFailed, "index count is not a multiple of 3", context);
    }
    const std::size_t vertexCount = mesh.vertices.size();
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        if (mesh.indices[i] >= vertexCount) {
            return makeError(ErrorCode::ValidationFailed, "index " + std::to_string(i) + " is out of range", context);
        }
    }
    for (std::size_t v = 0; v < vertexCount; ++v) {
        const Vertex& vertex = mesh.vertices[v];
        if (!core::isFinite(vertex.position) || !core::isFinite(vertex.normal) || !core::isFinite(vertex.tangent) ||
            !std::isfinite(vertex.uv0.x) || !std::isfinite(vertex.uv0.y)) {
            return makeError(ErrorCode::ValidationFailed, "vertex " + std::to_string(v) + " has non-finite data",
                             context);
        }
        if (glm::dot(vertex.normal, vertex.normal) < 1e-12f) {
            return makeError(ErrorCode::ValidationFailed, "vertex " + std::to_string(v) + " has a zero normal",
                             context);
        }
    }
    return ok();
}

std::uint64_t hashMesh(const MeshData& mesh) noexcept {
    constexpr std::uint64_t kOffset = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = kOffset;
    const auto feed = [&hash](const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= kPrime;
        }
    };
    const std::uint64_t vertexCount = mesh.vertices.size();
    const std::uint64_t indexCount = mesh.indices.size();
    feed(&vertexCount, sizeof(vertexCount));
    feed(&indexCount, sizeof(indexCount));
    if (!mesh.vertices.empty()) {
        feed(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex));
    }
    if (!mesh.indices.empty()) {
        feed(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
    }
    return hash;
}

} // namespace rv::geometry
