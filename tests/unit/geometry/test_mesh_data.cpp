#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include <rv/geometry/mesh_data.hpp>
#include <rv/geometry/socket.hpp>

using Catch::Approx;
using rv::geometry::MeshData;
using rv::geometry::Vertex;

namespace {

/// Unit quad in the XY plane facing +Z, uv (0,0) at the top-left corner (ADR-0002).
MeshData makeQuad() {
    MeshData mesh;
    mesh.name = "quad";
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    mesh.vertices = {
        Vertex{{-0.5f, 0.5f, 0.0f}, n, {}, {0.0f, 0.0f}},  // top-left
        Vertex{{-0.5f, -0.5f, 0.0f}, n, {}, {0.0f, 1.0f}}, // bottom-left
        Vertex{{0.5f, -0.5f, 0.0f}, n, {}, {1.0f, 1.0f}},  // bottom-right
        Vertex{{0.5f, 0.5f, 0.0f}, n, {}, {1.0f, 0.0f}},   // top-right
    };
    mesh.indices = {0, 1, 2, 0, 2, 3};
    return mesh;
}

} // namespace

TEST_CASE("geometry/mesh_data: standard vertex layout", "[geometry][mesh]") {
    const auto& layout = rv::geometry::VertexLayout::standard();
    CHECK(layout.stride == 48);
    CHECK(layout.attributes[0].location == 0);
    CHECK(layout.attributes[0].components == 3);
    CHECK(layout.attributes[1].offset == 12);
    CHECK(layout.attributes[2].components == 4);
    CHECK(layout.attributes[2].offset == 24);
    CHECK(layout.attributes[3].offset == 40);
}

TEST_CASE("geometry/mesh_data: bounds, tangents, validation", "[geometry][mesh]") {
    MeshData quad = makeQuad();
    rv::geometry::computeBounds(quad);
    CHECK(quad.bounds.size() == glm::vec3(1.0f, 1.0f, 0.0f));
    CHECK(quad.triangleCount() == 2);

    rv::geometry::computeTangents(quad);
    for (const Vertex& v : quad.vertices) {
        // u grows towards +X, v towards -Y: tangent = +X; the glTF bitangent cross(N, T) * w follows -v, i.e. +Y
        // (the top of the image) => handedness +1 with N = +Z.
        CHECK(v.tangent.x == Approx(1.0f));
        CHECK(v.tangent.y == Approx(0.0f).margin(1e-6));
        CHECK(v.tangent.z == Approx(0.0f).margin(1e-6));
        CHECK(v.tangent.w == 1.0f);
    }
    CHECK(rv::geometry::validateMesh(quad).has_value());

    MeshData broken = makeQuad();
    broken.indices.push_back(7);
    CHECK_FALSE(rv::geometry::validateMesh(broken).has_value());
    broken.indices.push_back(0);
    broken.indices.push_back(1);
    CHECK(rv::geometry::validateMesh(broken).error().code == rv::ErrorCode::ValidationFailed);

    MeshData zeroNormal = makeQuad();
    zeroNormal.vertices[2].normal = glm::vec3(0.0f);
    CHECK_FALSE(rv::geometry::validateMesh(zeroNormal).has_value());
}

TEST_CASE("geometry/mesh_data: degenerate UVs still give a perpendicular unit tangent", "[geometry][mesh]") {
    MeshData mesh = makeQuad();
    for (Vertex& v : mesh.vertices) {
        v.uv0 = glm::vec2(0.5f);
    }
    rv::geometry::computeTangents(mesh);
    for (const Vertex& v : mesh.vertices) {
        const glm::vec3 t(v.tangent);
        CHECK(glm::length(t) == Approx(1.0f));
        CHECK(glm::dot(t, v.normal) == Approx(0.0f).margin(1e-6));
    }
}

TEST_CASE("geometry/mesh_data: uv scale gives the aspect ratio of the textured surface", "[geometry][mesh]") {
    MeshData quad = makeQuad();
    CHECK(rv::geometry::uvMetersPerUnit(quad) == glm::vec2(1.0f, 1.0f));

    // A 0.4 x 0.3 m label with uv 0..1: an undistorted image is 4:3.
    for (Vertex& v : quad.vertices) {
        v.position *= glm::vec3(0.4f, 0.3f, 1.0f);
    }
    glm::vec2 scale = rv::geometry::uvMetersPerUnit(quad);
    CHECK(scale.x == Approx(0.4f));
    CHECK(scale.y == Approx(0.3f));

    // Tiling twice along u halves the meters per unit along u; turning the surface does not matter.
    for (Vertex& v : quad.vertices) {
        v.uv0.x *= 2.0f;
        v.position = glm::vec3(v.position.z, v.position.y, -v.position.x);
    }
    scale = rv::geometry::uvMetersPerUnit(quad);
    CHECK(scale.x == Approx(0.2f));
    CHECK(scale.y == Approx(0.3f));

    // Degenerate uvs, out-of-range indices and empty meshes give no scale instead of NaN.
    MeshData degenerate = makeQuad();
    for (Vertex& v : degenerate.vertices) {
        v.uv0 = glm::vec2(0.5f);
    }
    CHECK(rv::geometry::uvMetersPerUnit(degenerate) == glm::vec2(0.0f));
    MeshData outOfRange = makeQuad();
    outOfRange.indices = {0, 1, 9};
    CHECK(rv::geometry::uvMetersPerUnit(outOfRange) == glm::vec2(0.0f));
    CHECK(rv::geometry::uvMetersPerUnit(MeshData{}) == glm::vec2(0.0f));
}

TEST_CASE("geometry/mesh_data: hash is deterministic and content-sensitive", "[geometry][mesh]") {
    const MeshData a = makeQuad();
    MeshData b = makeQuad();
    CHECK(rv::geometry::hashMesh(a) == rv::geometry::hashMesh(b));
    b.vertices[0].uv0.x = 0.001f;
    CHECK(rv::geometry::hashMesh(a) != rv::geometry::hashMesh(b));
}

TEST_CASE("geometry/socket: lookup by id", "[geometry][socket]") {
    std::vector<rv::geometry::Socket> sockets(2);
    sockets[0].id = "TV_Stand";
    sockets[0].kind = rv::geometry::SocketKind::Anchor;
    sockets[1].id = "ScreenSurface";
    sockets[1].kind = rv::geometry::SocketKind::ScreenSurface;
    const auto* screen = rv::geometry::findSocket(sockets, "ScreenSurface");
    REQUIRE(screen != nullptr);
    CHECK(screen->kind == rv::geometry::SocketKind::ScreenSurface);
    CHECK(rv::geometry::findSocket(sockets, "missing") == nullptr);
    CHECK(rv::geometry::toString(rv::geometry::SocketKind::ShelfSlot) == "shelf-slot");
}

TEST_CASE("geometry/mesh_data: generated tangents follow the glTF normal-map convention", "[geometry][mesh]") {
    // ADR-0002: bitangent = cross(normal, tangent.xyz) * tangent.w. glTF normal maps store tangent-space +X along +u
    // (right) and +Y up in the image; with uv (0, 0) at the top-left corner, "up" is the direction of decreasing v.
    // A normal-map texel tilted up must therefore tilt the shading normal towards the top of the picture, as in the
    // Khronos sample viewer, whichever way the quad is mirrored.
    const auto shade = [](const Vertex& v, const glm::vec3& tangentSpace) {
        const glm::vec3 t(v.tangent);
        const glm::vec3 b = glm::cross(v.normal, t) * v.tangent.w;
        return glm::normalize(t * tangentSpace.x + b * tangentSpace.y + v.normal * tangentSpace.z);
    };
    const glm::vec3 tiltedUp(0.0f, 0.6f, 0.8f);
    const glm::vec3 tiltedRight(0.6f, 0.0f, 0.8f);

    MeshData quad = makeQuad(); // facing +Z, image top at +Y, image right at +X
    rv::geometry::computeTangents(quad);
    for (const Vertex& v : quad.vertices) {
        CHECK(shade(v, tiltedUp).y == Approx(0.6f));
        CHECK(shade(v, tiltedRight).x == Approx(0.6f));
    }

    // The same picture mirrored left-right (u grows towards -X): "right" in the image is now -X, "up" is still +Y.
    MeshData mirrored = makeQuad();
    for (Vertex& v : mirrored.vertices) {
        v.uv0.x = 1.0f - v.uv0.x;
    }
    rv::geometry::computeTangents(mirrored);
    for (const Vertex& v : mirrored.vertices) {
        CHECK(v.tangent.w == -1.0f);
        CHECK(shade(v, tiltedUp).y == Approx(0.6f));
        CHECK(shade(v, tiltedRight).x == Approx(-0.6f));
    }
}
