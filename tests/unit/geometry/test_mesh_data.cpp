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
        // u grows towards +X, v grows towards -Y: tangent = +X, bitangent = -Y => handedness -1 with N = +Z.
        CHECK(v.tangent.x == Approx(1.0f));
        CHECK(v.tangent.y == Approx(0.0f).margin(1e-6));
        CHECK(v.tangent.z == Approx(0.0f).margin(1e-6));
        CHECK(std::abs(v.tangent.w) == Approx(1.0f));
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
