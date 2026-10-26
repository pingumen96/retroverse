#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/geometry/mesh_builder.hpp>
#include <rv/geometry/mesh_data.hpp>

#include "mesh_checks.hpp"

using Catch::Approx;
using rv::geometry::BoxUv;
using rv::geometry::FaceMask;
using rv::geometry::MeshBuilder;
using rv::geometry::MeshData;
using rv::geometry::Vertex;

namespace {

const glm::mat4 kIdentity(1.0f);

glm::mat4 translate(float x, float y, float z) {
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
}

glm::mat4 rotate(float degrees, const glm::vec3& axis) {
    return glm::rotate(glm::mat4(1.0f), glm::radians(degrees), axis);
}

glm::mat4 scale(float x, float y, float z) {
    return glm::scale(glm::mat4(1.0f), glm::vec3(x, y, z));
}

bool closeTo(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-5f) {
    return rv::core::nearlyEqual(a, b, epsilon);
}

bool closeTo(const glm::vec2& a, const glm::vec2& b, float epsilon = 1e-5f) {
    return rv::core::nearlyEqual(a.x, b.x, epsilon) && rv::core::nearlyEqual(a.y, b.y, epsilon);
}

MeshData buildBox(const glm::vec3& size, const glm::mat4& transform = kIdentity, BoxUv uv = BoxUv::PerFace,
                  float metersPerTile = 1.0f, FaceMask faces = FaceMask::All) {
    MeshBuilder builder;
    builder.box(size, transform, uv, metersPerTile, faces);
    return std::move(builder).build("box");
}

/// Triangles of a closed convex shape around `center` whose winding points inwards.
std::size_t inwardTriangles(const MeshData& mesh, const glm::vec3& center) {
    std::size_t count = 0;
    for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
        const rv::test::TriangleRef tri = rv::test::triangleAt(mesh, t);
        const glm::vec3 g = tri.cross();
        if (glm::length(g) > 1e-10f && glm::dot(g, tri.centroid() - center) <= 0.0f) {
            ++count;
        }
    }
    return count;
}

/// Expected box face frames from the contract: seen from outside, "up" is +Y on the sides, -Z on the top face and
/// +Z on the bottom face; "right" = up x normal.
struct FaceFrame {
    glm::vec3 normal;
    glm::vec3 right;
    glm::vec3 up;
};

const std::array<FaceFrame, 6> kBoxFaces{{
    {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f, 0.0f}},
    {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
    {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
    {{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
    {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
    {{0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
}};

const Vertex* findVertex(const MeshData& mesh, const glm::vec3& normal, const glm::vec3& position) {
    for (const Vertex& v : mesh.vertices) {
        if (closeTo(v.normal, normal) && closeTo(v.position, position)) {
            return &v;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("geometry/mesh_builder: box has 24 vertices, 36 indices and outward unit normals",
          "[geometry][mesh_builder]") {
    const glm::vec3 size(2.0f, 1.0f, 0.5f);
    const MeshData mesh = buildBox(size);
    CHECK(mesh.name == "box");
    CHECK(mesh.vertices.size() == 24);
    CHECK(mesh.indices.size() == 36);
    CHECK(rv::geometry::validateMesh(mesh).has_value());
    CHECK(mesh.bounds.min == -0.5f * size);
    CHECK(mesh.bounds.max == 0.5f * size);
    CHECK(rv::test::nonUnitNormals(mesh) == 0);
    CHECK(rv::test::windingMismatches(mesh) == 0);
    CHECK(inwardTriangles(mesh, glm::vec3(0.0f)) == 0);
    for (const FaceFrame& face : kBoxFaces) {
        std::size_t count = 0;
        for (const Vertex& v : mesh.vertices) {
            if (v.normal == face.normal) {
                ++count;
                // Every vertex of the face lies on the face plane.
                CHECK(glm::dot(v.position, face.normal) == Approx(glm::dot(glm::abs(face.normal), 0.5f * size)));
            }
        }
        CHECK(count == 4);
    }
}

TEST_CASE("geometry/mesh_builder: box faces put uv (0,0) at their top-left corner seen from outside",
          "[geometry][mesh_builder][uv]") {
    const glm::vec3 size(2.0f, 1.0f, 0.5f);
    const glm::vec3 h = 0.5f * size;
    const MeshData mesh = buildBox(size);
    for (const FaceFrame& face : kBoxFaces) {
        const float halfRight = glm::dot(glm::abs(face.right), h);
        const float halfUp = glm::dot(glm::abs(face.up), h);
        for (const Vertex& v : mesh.vertices) {
            if (v.normal != face.normal) {
                continue;
            }
            const glm::vec2 expected(0.5f + glm::dot(v.position, face.right) / (2.0f * halfRight),
                                     0.5f - glm::dot(v.position, face.up) / (2.0f * halfUp));
            CHECK(closeTo(v.uv0, expected));
        }
    }
    // Explicit corners: front face, top face (image top towards -Z) and bottom face (image top towards +Z).
    const Vertex* frontTopLeft = findVertex(mesh, {0.0f, 0.0f, 1.0f}, {-h.x, h.y, h.z});
    const Vertex* frontBottomRight = findVertex(mesh, {0.0f, 0.0f, 1.0f}, {h.x, -h.y, h.z});
    const Vertex* topTopLeft = findVertex(mesh, {0.0f, 1.0f, 0.0f}, {-h.x, h.y, -h.z});
    const Vertex* bottomTopLeft = findVertex(mesh, {0.0f, -1.0f, 0.0f}, {-h.x, -h.y, h.z});
    const Vertex* rightTopLeft = findVertex(mesh, {1.0f, 0.0f, 0.0f}, {h.x, h.y, h.z});
    const Vertex* backTopLeft = findVertex(mesh, {0.0f, 0.0f, -1.0f}, {h.x, h.y, -h.z});
    REQUIRE(frontTopLeft != nullptr);
    REQUIRE(frontBottomRight != nullptr);
    REQUIRE(topTopLeft != nullptr);
    REQUIRE(bottomTopLeft != nullptr);
    REQUIRE(rightTopLeft != nullptr);
    REQUIRE(backTopLeft != nullptr);
    CHECK(frontTopLeft->uv0 == glm::vec2(0.0f, 0.0f));
    CHECK(frontBottomRight->uv0 == glm::vec2(1.0f, 1.0f));
    CHECK(topTopLeft->uv0 == glm::vec2(0.0f, 0.0f));
    CHECK(bottomTopLeft->uv0 == glm::vec2(0.0f, 0.0f));
    CHECK(rightTopLeft->uv0 == glm::vec2(0.0f, 0.0f));
    CHECK(backTopLeft->uv0 == glm::vec2(0.0f, 0.0f));
}

TEST_CASE("geometry/mesh_builder: world-meter uvs tile seamlessly across adjacent boxes",
          "[geometry][mesh_builder][uv]") {
    MeshBuilder builder;
    builder.box({1.0f, 2.0f, 0.2f}, translate(0.5f, 1.0f, 0.0f), BoxUv::WorldMeters, 0.5f, FaceMask::PosZ);
    builder.box({1.0f, 2.0f, 0.2f}, translate(1.5f, 1.0f, 0.0f), BoxUv::WorldMeters, 0.5f, FaceMask::PosZ);
    const MeshData wall = std::move(builder).build("wall");
    REQUIRE(wall.vertices.size() == 8);
    for (const Vertex& v : wall.vertices) {
        // Front faces: u along +X, v downwards, both in tiles of 0.5 m.
        CHECK(closeTo(v.uv0, glm::vec2(v.position.x / 0.5f, -v.position.y / 0.5f)));
    }

    // A rotated box keeps the uv axes on the transformed face: the front face now looks along +X and its
    // right axis is -Z.
    const MeshData turned =
        buildBox({1.0f, 1.0f, 1.0f}, translate(3.0f, 0.5f, 2.0f) * rotate(90.0f, {0.0f, 1.0f, 0.0f}),
                 BoxUv::WorldMeters, 1.0f, FaceMask::PosZ);
    REQUIRE(turned.vertices.size() == 4);
    for (const Vertex& v : turned.vertices) {
        CHECK(closeTo(v.normal, glm::vec3(1.0f, 0.0f, 0.0f)));
        CHECK(closeTo(v.uv0, glm::vec2(-v.position.z, -v.position.y)));
    }

    // Non-positive tile sizes fall back to 1 m.
    const MeshData fallback = buildBox({1.0f, 1.0f, 1.0f}, kIdentity, BoxUv::WorldMeters, 0.0f, FaceMask::PosZ);
    for (const Vertex& v : fallback.vertices) {
        CHECK(closeTo(v.uv0, glm::vec2(v.position.x, -v.position.y)));
    }
}

TEST_CASE("geometry/mesh_builder: grain-meter uvs run along the longer side of every face",
          "[geometry][mesh_builder][uv]") {
    // A tall board (0.3 m wide, 2 m tall, 2 cm deep) centered at (1, 1, 0). Its front face is taller than wide: u runs
    // down it and v towards its left (the WorldMeters layout turned a quarter turn); its top face is wider than deep:
    // the WorldMeters layout itself.
    const glm::mat4 placement = translate(1.0f, 1.0f, 0.0f);
    const MeshData board = buildBox({0.3f, 2.0f, 0.02f}, placement, BoxUv::GrainMeters, 0.5f,
                                    FaceMask::PosZ | FaceMask::PosY);
    REQUIRE(board.vertices.size() == 8);
    for (const Vertex& v : board.vertices) {
        INFO(v.position.x << " " << v.position.y << " " << v.position.z);
        if (v.normal.z > 0.5f) {
            CHECK(closeTo(v.uv0, glm::vec2(-v.position.y, -v.position.x) / 0.5f));
        } else {
            CHECK(closeTo(v.uv0, glm::vec2(v.position.x, v.position.z) / 0.5f));
        }
    }
    // A quarter turn, never a mirror: the tangent frames stay consistent with the uvs.
    CHECK(rv::test::tangentFrameMismatches(board) == 0);
    CHECK(rv::test::badTangents(board) == 0);

    // Chamfered boards map their flat faces the same way, one uv unit per metersPerTile, and their bevels lean on the
    // faces' mapping; below the smallest chamfer they are the plain box with the same layout.
    MeshBuilder chamfered;
    chamfered.chamferedBox({0.3f, 2.0f, 0.02f}, 0.003f, placement, BoxUv::GrainMeters, 1.0f);
    const MeshData bevelled = std::move(chamfered).build("bevelled");
    for (const Vertex& v : bevelled.vertices) {
        if (closeTo(v.normal, glm::vec3(0.0f, 0.0f, 1.0f))) {
            CHECK(closeTo(v.uv0, glm::vec2(-v.position.y, -v.position.x)));
        }
    }
    CHECK(rv::test::tangentFrameMismatches(bevelled) == 0);
    MeshBuilder tiny;
    tiny.chamferedBox({0.3f, 2.0f, 0.02f}, 1e-7f, placement, BoxUv::GrainMeters, 1.0f);
    CHECK(rv::geometry::hashMesh(std::move(tiny).build("box")) ==
          rv::geometry::hashMesh(buildBox({0.3f, 2.0f, 0.02f}, placement, BoxUv::GrainMeters, 1.0f)));
    // World meters on a chamfered box follow the face axes too; per-face uvs stay the default.
    MeshBuilder world;
    world.chamferedBox({1.0f, 1.0f, 1.0f}, 0.05f, translate(2.0f, 0.0f, 0.0f), BoxUv::WorldMeters, 0.5f);
    for (const Vertex& v : std::move(world).build("world").vertices) {
        if (closeTo(v.normal, glm::vec3(0.0f, 0.0f, 1.0f))) {
            CHECK(closeTo(v.uv0, glm::vec2(v.position.x, -v.position.y) / 0.5f));
        }
    }
}

TEST_CASE("geometry/mesh_builder: face mask drops faces", "[geometry][mesh_builder]") {
    const MeshData noBottom =
        buildBox({1.0f, 1.0f, 1.0f}, kIdentity, BoxUv::PerFace, 1.0f, FaceMask::All & ~FaceMask::NegY);
    CHECK(noBottom.vertices.size() == 20);
    CHECK(noBottom.indices.size() == 30);
    for (const Vertex& v : noBottom.vertices) {
        CHECK(v.normal != glm::vec3(0.0f, -1.0f, 0.0f));
    }
    const MeshData top = buildBox({1.0f, 1.0f, 1.0f}, kIdentity, BoxUv::PerFace, 1.0f, FaceMask::PosY);
    CHECK(top.vertices.size() == 4);
    CHECK(top.bounds.min.y == 0.5f);

    MeshBuilder none;
    none.box({1.0f, 1.0f, 1.0f}, kIdentity, BoxUv::PerFace, 1.0f, FaceMask::None);
    CHECK(none.empty());
    const MeshData empty = std::move(none).build("empty");
    CHECK(empty.empty());
    CHECK(empty.bounds.isEmpty());

    CHECK((FaceMask::PosX | FaceMask::NegX) ==
          (FaceMask::All & ~(FaceMask::PosY | FaceMask::NegY | FaceMask::PosZ | FaceMask::NegZ)));
    CHECK(rv::geometry::hasAny(FaceMask::All, FaceMask::NegZ));
    CHECK_FALSE(rv::geometry::hasAny(~FaceMask::All, FaceMask::All));
}

TEST_CASE("geometry/mesh_builder: normals follow the inverse-transpose of non-uniform transforms",
          "[geometry][mesh_builder][transform]") {
    // A rotated box scaled along X: its faces are no longer at 45 degrees, so naively transformed normals
    // would not be perpendicular to them.
    const glm::mat4 transform =
        translate(1.0f, 2.0f, 3.0f) * scale(2.0f, 1.0f, 1.0f) * rotate(45.0f, {0.0f, 0.0f, 1.0f});
    for (const MeshData& mesh : {buildBox({1.0f, 1.0f, 1.0f}, transform), [&] {
                                     MeshBuilder builder;
                                     builder.chamferedBox({1.0f, 0.6f, 0.4f}, 0.05f, transform);
                                     return std::move(builder).build("chamfered");
                                 }()}) {
        CHECK(rv::test::nonUnitNormals(mesh) == 0);
        CHECK(rv::test::windingMismatches(mesh) == 0);
        CHECK(inwardTriangles(mesh, glm::vec3(1.0f, 2.0f, 3.0f)) == 0);
        for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
            const rv::test::TriangleRef tri = rv::test::triangleAt(mesh, t);
            const glm::vec3 faceNormal = glm::normalize(tri.cross());
            CHECK(closeTo(tri.v0.normal, faceNormal, 1e-4f));
            CHECK(closeTo(tri.v2.normal, faceNormal, 1e-4f));
        }
    }
}

TEST_CASE("geometry/mesh_builder: mirroring transforms keep faces counter-clockwise and outward",
          "[geometry][mesh_builder][transform]") {
    const glm::vec3 center(0.5f, 1.0f, -2.0f);
    for (const glm::mat4& mirror : {scale(-1.0f, 1.0f, 1.0f), scale(1.0f, 1.0f, -2.0f), scale(-1.0f, -1.0f, -1.0f)}) {
        const glm::mat4 transform =
            glm::translate(glm::mat4(1.0f), center) * mirror * rotate(30.0f, {0.0f, 1.0f, 0.0f});
        MeshBuilder closed;
        closed.box({1.0f, 0.5f, 0.25f}, transform);
        const MeshData box = std::move(closed).build("box");
        MeshBuilder chamfered;
        chamfered.chamferedBox({1.0f, 0.5f, 0.25f}, 0.04f, transform);
        MeshBuilder sphere;
        sphere.uvSphere(0.5f, 12, 6, transform);
        MeshBuilder cylinder;
        cylinder.cylinder(0.3f, 0.8f, 10, true, transform);
        MeshBuilder open;
        open.quad({1.0f, 0.5f}, transform).plane({0.4f, 0.3f}, transform);
        for (const MeshData& mesh : {box, std::move(chamfered).build("chamfered"), std::move(sphere).build("sphere"),
                                     std::move(cylinder).build("cylinder")}) {
            CHECK(rv::test::windingMismatches(mesh) == 0);
            CHECK(inwardTriangles(mesh, center) == 0);
            CHECK(rv::test::badTangents(mesh) == 0);
            CHECK(rv::test::tangentFrameMismatches(mesh) == 0);
        }
        const MeshData flat = std::move(open).build("open");
        CHECK(rv::test::windingMismatches(flat) == 0);
        CHECK(rv::test::tangentFrameMismatches(flat) == 0);
    }
    // A mirror flips the order of two indices per triangle compared to the plain box.
    const MeshData plain = buildBox({1.0f, 1.0f, 1.0f});
    const MeshData mirrored = buildBox({1.0f, 1.0f, 1.0f}, scale(-1.0f, 1.0f, 1.0f));
    REQUIRE(plain.indices.size() == mirrored.indices.size());
    CHECK(plain.indices[0] == mirrored.indices[0]);
    CHECK(plain.indices[1] == mirrored.indices[2]);
    CHECK(plain.indices[2] == mirrored.indices[1]);
}

TEST_CASE("geometry/mesh_builder: chamfered box bevels every edge", "[geometry][mesh_builder][chamfer]") {
    const glm::vec3 size(0.125f, 0.085f, 0.02f);
    const float chamfer = 0.002f;
    MeshBuilder builder;
    builder.chamferedBox(size, chamfer);
    const MeshData mesh = std::move(builder).build("cartridge_shell");
    CHECK(mesh.vertices.size() == 96);
    CHECK(mesh.indices.size() == 132);
    CHECK(rv::geometry::validateMesh(mesh).has_value());
    CHECK(closeTo(mesh.bounds.min, -0.5f * size, 1e-7f));
    CHECK(closeTo(mesh.bounds.max, 0.5f * size, 1e-7f));
    CHECK(rv::test::windingMismatches(mesh) == 0);
    CHECK(inwardTriangles(mesh, glm::vec3(0.0f)) == 0);

    // 6 faces x 4 axis normals, 12 strips x 4 normals at 45 degrees, 8 corners x 3 normals along a diagonal.
    std::size_t faceVertices = 0;
    std::size_t bevelVertices = 0;
    std::size_t cornerVertices = 0;
    const float diagonal2 = 1.0f / std::sqrt(2.0f);
    const float diagonal3 = 1.0f / std::sqrt(3.0f);
    for (const Vertex& v : mesh.vertices) {
        const glm::vec3 a = glm::abs(v.normal);
        const int zeros = static_cast<int>(a.x < 1e-6f) + static_cast<int>(a.y < 1e-6f) + static_cast<int>(a.z < 1e-6f);
        if (zeros == 2) {
            ++faceVertices;
        } else if (zeros == 1) {
            ++bevelVertices;
            CHECK(std::max({a.x, a.y, a.z}) == Approx(diagonal2));
        } else {
            ++cornerVertices;
            CHECK(a.x == Approx(diagonal3));
            CHECK(a.y == Approx(diagonal3));
        }
        // Face vertices sit chamfer meters inside the edges along the face.
        if (zeros == 2 && a.z > 0.5f) {
            CHECK(std::fabs(v.position.x) == Approx(0.5f * size.x - chamfer));
            CHECK(std::fabs(v.position.y) == Approx(0.5f * size.y - chamfer));
        }
    }
    CHECK(faceVertices == 24);
    CHECK(bevelVertices == 48);
    CHECK(cornerVertices == 24);

    MeshBuilder sharp;
    sharp.chamferedBox(size, 0.0f);
    CHECK(std::move(sharp).build("sharp").vertices.size() == 24);

    // An oversized chamfer is clamped instead of turning the box inside out.
    MeshBuilder clamped;
    clamped.chamferedBox({1.0f, 0.1f, 1.0f}, 5.0f);
    const MeshData rounded = std::move(clamped).build("clamped");
    CHECK(rounded.vertices.size() == 96);
    CHECK(closeTo(rounded.bounds.size(), glm::vec3(1.0f, 0.1f, 1.0f)));
    CHECK(rv::test::windingMismatches(rounded) == 0);
    CHECK(inwardTriangles(rounded, glm::vec3(0.0f)) == 0);
}

TEST_CASE("geometry/mesh_builder: a chamfered box can bevel the edges of some faces only",
          "[geometry][mesh_builder][chamfer]") {
    // Volume enclosed by a closed mesh with outward triangles (divergence theorem); holes would change it.
    const auto enclosedVolume = [](const MeshData& mesh) {
        double volume = 0.0;
        for (std::size_t t = 0; t < mesh.triangleCount(); ++t) {
            const rv::test::TriangleRef tri = rv::test::triangleAt(mesh, t);
            volume += static_cast<double>(glm::dot(tri.v0.position, glm::cross(tri.v1.position, tri.v2.position)));
        }
        return volume / 6.0;
    };
    const glm::vec3 size(0.1f, 0.05f, 0.08f);
    const float c = 0.01f;

    // The top face's four edges: six faces and four strips; at the top corners the strips meet in mitres (no corner
    // triangle) above square vertical edges, and the bottom face keeps its full size.
    MeshBuilder top;
    top.chamferedBox(size, c, kIdentity, BoxUv::PerFace, 1.0f, FaceMask::PosY);
    const MeshData board = std::move(top).build("board");
    CHECK(board.vertices.size() == 6 * 4 + 4 * 4);
    CHECK(board.indices.size() == 6 * 6 + 4 * 6);
    CHECK(rv::geometry::validateMesh(board).has_value());
    CHECK(closeTo(board.bounds.min, -0.5f * size, 1e-7f));
    CHECK(closeTo(board.bounds.max, 0.5f * size, 1e-7f));
    CHECK(rv::test::windingMismatches(board) == 0);
    CHECK(inwardTriangles(board, glm::vec3(0.0f)) == 0);
    CHECK(rv::test::tangentFrameMismatches(board) == 0);
    // Closed: the box less the bevels along the four top edges, whose mitred ends overlap at the top corners (c^3 / 3
    // each).
    const double boxVolume = static_cast<double>(size.x) * size.y * size.z;
    const double bevels = static_cast<double>(c) * c * (size.x + size.z) - 4.0 * c * c * c / 3.0;
    CHECK(enclosedVolume(board) == Approx(boxVolume - bevels).epsilon(1e-4));
    for (const Vertex& v : board.vertices) {
        // Nothing leans downwards: the bottom edges stay square.
        if (v.normal.y < 0.0f) {
            CHECK(closeTo(v.normal, glm::vec3(0.0f, -1.0f, 0.0f)));
            CHECK(std::fabs(v.position.x) == Approx(0.5f * size.x));
            CHECK(std::fabs(v.position.z) == Approx(0.5f * size.z));
        }
        // The top face is inset by the chamfer on all four sides.
        if (closeTo(v.normal, glm::vec3(0.0f, 1.0f, 0.0f))) {
            CHECK(std::fabs(v.position.x) == Approx(0.5f * size.x - c));
            CHECK(std::fabs(v.position.z) == Approx(0.5f * size.z - c));
        }
    }

    // Two faces: the seven edges around them; the two corners where all three edges are bevelled get triangles.
    MeshBuilder frontTop;
    frontTop.chamferedBox(size, c, kIdentity, BoxUv::PerFace, 1.0f, FaceMask::PosY | FaceMask::PosZ);
    const MeshData edged = std::move(frontTop).build("edged");
    CHECK(edged.vertices.size() == 6 * 4 + 7 * 4 + 2 * 3);
    CHECK(rv::test::windingMismatches(edged) == 0);
    CHECK(inwardTriangles(edged, glm::vec3(0.0f)) == 0);
    CHECK(enclosedVolume(edged) < enclosedVolume(board));

    // Every face bevels all twelve edges, as the default does; none gives the plain box.
    const auto hashOf = [&](FaceMask bevelled) {
        MeshBuilder builder;
        builder.chamferedBox(size, c, kIdentity, BoxUv::PerFace, 1.0f, bevelled);
        return rv::geometry::hashMesh(std::move(builder).build("mesh"));
    };
    MeshBuilder defaults;
    defaults.chamferedBox(size, c);
    CHECK(hashOf(FaceMask::All) == rv::geometry::hashMesh(std::move(defaults).build("mesh")));
    CHECK(hashOf(FaceMask::None) == rv::geometry::hashMesh(buildBox(size)));
}

TEST_CASE("geometry/mesh_builder: plane and quad orientation and uvs", "[geometry][mesh_builder][uv]") {
    MeshBuilder planeBuilder;
    planeBuilder.plane({4.0f, 2.0f}, kIdentity, {8.0f, 4.0f});
    const MeshData plane = std::move(planeBuilder).build("plane");
    REQUIRE(plane.vertices.size() == 4);
    CHECK(plane.indices.size() == 6);
    CHECK(rv::test::windingMismatches(plane) == 0);
    CHECK(plane.bounds.size() == glm::vec3(4.0f, 0.0f, 2.0f));
    for (const Vertex& v : plane.vertices) {
        CHECK(v.normal == glm::vec3(0.0f, 1.0f, 0.0f));
        // (0, 0) at the -X/-Z corner, u along +X, v along +Z, tiled 8 x 4.
        CHECK(closeTo(v.uv0, glm::vec2((v.position.x + 2.0f) * 2.0f, (v.position.z + 1.0f) * 2.0f)));
    }

    MeshBuilder quadBuilder;
    quadBuilder.quad({2.0f, 1.0f}, translate(0.0f, 0.0f, 0.3f));
    quadBuilder.quad({2.0f, 1.0f}, translate(0.0f, 0.0f, 0.6f), {0.25f, 0.5f, 0.75f, 1.0f});
    const MeshData quads = std::move(quadBuilder).build("quads");
    REQUIRE(quads.vertices.size() == 8);
    CHECK(rv::test::windingMismatches(quads) == 0);
    for (std::size_t i = 0; i < quads.vertices.size(); ++i) {
        const Vertex& v = quads.vertices[i];
        CHECK(v.normal == glm::vec3(0.0f, 0.0f, 1.0f));
        const bool left = v.position.x < 0.0f;
        const bool top = v.position.y > 0.0f;
        if (i < 4) {
            CHECK(v.uv0 == glm::vec2(left ? 0.0f : 1.0f, top ? 0.0f : 1.0f));
        } else {
            CHECK(v.uv0 == glm::vec2(left ? 0.25f : 0.75f, top ? 0.5f : 1.0f));
        }
    }
    // The tangent follows +u (towards +X) and the bitangent -v (upwards, glTF tangent space): handedness +1 with
    // normal +Z.
    for (const Vertex& v : quads.vertices) {
        CHECK(closeTo(glm::vec3(v.tangent), glm::vec3(1.0f, 0.0f, 0.0f)));
        CHECK(v.tangent.w == 1.0f);
    }
}

TEST_CASE("geometry/mesh_builder: uv sphere has exact normals, a duplicated seam and sane poles",
          "[geometry][mesh_builder][sphere]") {
    const float radius = 0.5f;
    MeshBuilder builder;
    builder.uvSphere(radius, 32, 16);
    const MeshData sphere = std::move(builder).build("sphere");
    CHECK(sphere.vertices.size() == 2 * 32 + 15 * 33);
    CHECK(sphere.indices.size() == 6 * 32 * 15);
    CHECK(rv::geometry::validateMesh(sphere).has_value());
    CHECK(closeTo(sphere.bounds.min, glm::vec3(-radius), 1e-6f));
    CHECK(closeTo(sphere.bounds.max, glm::vec3(radius), 1e-6f));
    CHECK(rv::test::nonUnitNormals(sphere) == 0);
    CHECK(rv::test::windingMismatches(sphere) == 0);
    CHECK(inwardTriangles(sphere, glm::vec3(0.0f)) == 0);
    CHECK(rv::test::tangentFrameMismatches(sphere) == 0);

    std::size_t poles = 0;
    for (const Vertex& v : sphere.vertices) {
        CHECK(closeTo(v.normal, v.position / radius, 1e-5f));
        if (std::fabs(v.position.y) == radius) {
            ++poles;
            CHECK(std::fabs(v.normal.y) == 1.0f);
            CHECK((v.uv0.y == 0.0f) == (v.position.y > 0.0f));
        }
        // Seam column: u = 0 and u = 1 share the exact same position.
        if (v.uv0.x == 0.0f) {
            bool twin = false;
            for (const Vertex& w : sphere.vertices) {
                twin = twin || (w.uv0.x == 1.0f && w.uv0.y == v.uv0.y && w.position == v.position);
            }
            CHECK(twin);
        }
    }
    CHECK(poles == 64);

    // u = 0.5 faces +Z and u grows eastwards (towards +X on the front); v = 0.5 is the equator.
    const Vertex* front = findVertex(sphere, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, radius});
    const Vertex* east = findVertex(sphere, {1.0f, 0.0f, 0.0f}, {radius, 0.0f, 0.0f});
    REQUIRE(front != nullptr);
    REQUIRE(east != nullptr);
    CHECK(closeTo(front->uv0, glm::vec2(0.5f, 0.5f)));
    CHECK(closeTo(east->uv0, glm::vec2(0.75f, 0.5f)));

    MeshBuilder minimal;
    minimal.uvSphere(1.0f, -5, 0); // clamped to 3 segments and 2 rings
    const MeshData coarse = std::move(minimal).build("coarse");
    CHECK(coarse.vertices.size() == 2 * 3 + 1 * 4);
    CHECK(coarse.indices.size() == 6 * 3 * 1);
    CHECK(rv::test::windingMismatches(coarse) == 0);
}

TEST_CASE("geometry/mesh_builder: cylinders, cones and double-sided frusta", "[geometry][mesh_builder][frustum]") {
    MeshBuilder cappedBuilder;
    cappedBuilder.cylinder(0.25f, 1.0f, 32, true);
    const MeshData capped = std::move(cappedBuilder).build("capped");
    CHECK(capped.vertices.size() == 4 * 32 + 4);
    CHECK(capped.indices.size() == 12 * 32);
    CHECK(closeTo(capped.bounds.min, glm::vec3(-0.25f, -0.5f, -0.25f), 1e-6f));
    CHECK(closeTo(capped.bounds.max, glm::vec3(0.25f, 0.5f, 0.25f), 1e-6f));
    CHECK(rv::test::windingMismatches(capped) == 0);
    CHECK(inwardTriangles(capped, glm::vec3(0.0f)) == 0);
    CHECK(rv::test::tangentFrameMismatches(capped) == 0);
    std::size_t caps = 0;
    for (const Vertex& v : capped.vertices) {
        if (std::fabs(v.normal.y) == 1.0f) {
            ++caps;
        } else {
            CHECK(v.normal.y == Approx(0.0f).margin(1e-6));
            CHECK(closeTo(v.normal, glm::vec3(v.position.x, 0.0f, v.position.z) / 0.25f, 1e-5f));
        }
    }
    CHECK(caps == 2 * 33);

    MeshBuilder openBuilder;
    openBuilder.cylinder(0.25f, 1.0f, 32, false);
    const MeshData open = std::move(openBuilder).build("open");
    CHECK(open.vertices.size() == 66);
    CHECK(open.indices.size() == 192);

    // Cone: the apex has one vertex per segment and no degenerate triangle; its normals lean upwards.
    MeshBuilder coneBuilder;
    coneBuilder.frustum(0.5f, 0.0f, 1.0f, 16, true, false);
    const MeshData cone = std::move(coneBuilder).build("cone");
    CHECK(cone.vertices.size() == 3 * 16 + 2);
    CHECK(cone.indices.size() == 6 * 16);
    CHECK(rv::geometry::validateMesh(cone).has_value());
    CHECK(rv::test::windingMismatches(cone) == 0);
    CHECK(inwardTriangles(cone, glm::vec3(0.0f)) == 0);
    for (std::size_t t = 0; t < cone.triangleCount(); ++t) {
        CHECK(glm::length(rv::test::triangleAt(cone, t).cross()) > 1e-6f);
    }
    std::size_t apex = 0;
    for (const Vertex& v : cone.vertices) {
        if (v.position.y == 0.5f) {
            ++apex;
            CHECK(v.position == glm::vec3(0.0f, 0.5f, 0.0f));
            CHECK(v.normal.y == Approx(0.5f / std::sqrt(1.25f))); // slope of a 0.5 m radius, 1 m tall cone
            CHECK(glm::length(v.normal) == Approx(1.0f));
        }
    }
    CHECK(apex == 16);

    // Lamp shade: open and double-sided, the inside faces the axis.
    MeshBuilder shadeBuilder;
    shadeBuilder.frustum(0.22f, 0.16f, 0.3f, 24, false, true);
    const MeshData shade = std::move(shadeBuilder).build("shade");
    CHECK(shade.vertices.size() == 2 * 50);
    CHECK(shade.indices.size() == 2 * 144);
    CHECK(rv::test::windingMismatches(shade) == 0);
    CHECK(rv::test::tangentFrameMismatches(shade) == 0);
    std::size_t inward = 0;
    for (std::size_t t = 0; t < shade.triangleCount(); ++t) {
        const rv::test::TriangleRef tri = rv::test::triangleAt(shade, t);
        const glm::vec3 radial(tri.centroid().x, 0.0f, tri.centroid().z);
        inward += glm::dot(tri.cross(), radial) < 0.0f ? 1u : 0u;
    }
    CHECK(inward == shade.triangleCount() / 2);

    MeshBuilder degenerate;
    degenerate.frustum(0.0f, 0.0f, 1.0f, 16, true, true);
    CHECK(degenerate.empty());
}

TEST_CASE("geometry/mesh_builder: append transforms meshes and drops invalid triangles",
          "[geometry][mesh_builder][append]") {
    MeshData source;
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    source.vertices = {
        Vertex{{-0.5f, 0.5f, 0.0f}, n, {}, {0.0f, 0.0f}},
        Vertex{{-0.5f, -0.5f, 0.0f}, n, {}, {0.0f, 1.0f}},
        Vertex{{0.5f, -0.5f, 0.0f}, n, {}, {1.0f, 1.0f}},
        Vertex{{0.5f, 0.5f, 0.0f}, n, {}, {1.0f, 0.0f}},
    };
    // Two valid triangles, one referencing a missing vertex and a dangling pair of indices.
    source.indices = {0, 1, 2, 0, 2, 3, 0, 1, 7, 2, 3};

    MeshBuilder builder;
    builder.box({1.0f, 1.0f, 1.0f}).append(source, translate(5.0f, 0.0f, 0.0f));
    const MeshData mesh = std::move(builder).build("combined");
    CHECK(mesh.vertices.size() == 24 + 4);
    CHECK(mesh.indices.size() == 36 + 6);
    CHECK(rv::geometry::validateMesh(mesh).has_value());
    for (std::size_t i = 36; i < mesh.indices.size(); ++i) {
        CHECK(mesh.indices[i] >= 24);
        CHECK(mesh.vertices[mesh.indices[i]].position.x >= 4.5f);
    }
    CHECK(mesh.bounds.max.x == 5.5f);
    // build() recomputed the tangents of the appended quad from its uvs.
    CHECK(closeTo(glm::vec3(mesh.vertices[24].tangent), glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(mesh.vertices[24].tangent.w == 1.0f);

    // Mirrored placement: the winding is swapped so the quad still faces its normal.
    MeshBuilder mirrored;
    mirrored.append(source, scale(-1.0f, 1.0f, 1.0f));
    const MeshData flipped = std::move(mirrored).build("mirrored");
    CHECK(flipped.indices.size() == 6);
    CHECK(rv::test::windingMismatches(flipped) == 0);
    CHECK(rv::test::tangentFrameMismatches(flipped) == 0);

    MeshData broken = source;
    broken.indices = {4, 5, 6, 0};
    MeshBuilder nothing;
    nothing.append(broken);
    CHECK(nothing.empty());
}

TEST_CASE("geometry/mesh_builder: tangents follow the uv layout, degenerate uvs keep the carried tangent",
          "[geometry][mesh_builder][tangents]") {
    MeshBuilder builder;
    builder.box({1.0f, 2.0f, 3.0f}, rotate(20.0f, {1.0f, 1.0f, 0.0f}))
        .box({2.0f, 1.0f, 0.5f}, translate(3.0f, 0.0f, 0.0f), BoxUv::WorldMeters, 0.5f)
        .chamferedBox({0.4f, 0.3f, 0.2f}, 0.03f, translate(-2.0f, 0.0f, 0.0f))
        .plane({3.0f, 2.0f}, translate(0.0f, -2.0f, 0.0f), {3.0f, 2.0f})
        .uvSphere(0.5f, 16, 8, translate(0.0f, 3.0f, 0.0f))
        .frustum(0.3f, 0.1f, 0.5f, 12, true, true, translate(0.0f, 0.0f, 4.0f));
    const MeshData mesh = std::move(builder).build("mixed");
    CHECK(rv::test::badTangents(mesh) == 0);
    CHECK(rv::test::tangentFrameMismatches(mesh) == 0);

    const MeshData box = buildBox({1.0f, 1.0f, 1.0f});
    for (const Vertex& v : box.vertices) {
        for (const FaceFrame& face : kBoxFaces) {
            if (v.normal == face.normal) {
                CHECK(closeTo(glm::vec3(v.tangent), face.right));
                CHECK(v.tangent.w == 1.0f);
            }
        }
    }

    // A quad whose uvs collapse to one point has no uv frame: the tangent carried through the transform
    // (local +X, handedness +1) survives instead of an arbitrary perpendicular.
    const glm::vec4 collapsed(0.5f, 0.5f, 0.5f, 0.5f);
    MeshBuilder turned;
    turned.quad({1.0f, 1.0f}, rotate(90.0f, {0.0f, 0.0f, 1.0f}), collapsed);
    for (const Vertex& v : std::move(turned).build("turned").vertices) {
        CHECK(closeTo(glm::vec3(v.tangent), glm::vec3(0.0f, 1.0f, 0.0f)));
        CHECK(v.tangent.w == 1.0f);
    }
    MeshBuilder mirrored;
    mirrored.quad({1.0f, 1.0f}, scale(-1.0f, 1.0f, 1.0f), collapsed);
    for (const Vertex& v : std::move(mirrored).build("mirrored").vertices) {
        CHECK(closeTo(glm::vec3(v.tangent), glm::vec3(-1.0f, 0.0f, 0.0f)));
        CHECK(v.tangent.w == -1.0f);
    }
}

TEST_CASE("geometry/mesh_builder: identical input gives identical meshes", "[geometry][mesh_builder][determinism]") {
    const auto make = [](float radius) {
        MeshBuilder builder;
        builder.box({1.0f, 2.0f, 3.0f}, rotate(33.0f, {0.0f, 1.0f, 0.0f}), BoxUv::WorldMeters, 0.7f)
            .chamferedBox({0.3f, 0.2f, 0.1f}, 0.01f, translate(1.0f, 0.0f, 0.0f))
            .uvSphere(radius, 24, 12, translate(0.0f, 2.0f, 0.0f))
            .frustum(0.2f, 0.0f, 0.4f, 9, true, true, scale(-1.0f, 1.0f, 1.0f));
        return std::move(builder).build("deterministic");
    };
    const MeshData first = make(0.5f);
    const MeshData second = make(0.5f);
    CHECK(rv::geometry::hashMesh(first) == rv::geometry::hashMesh(second));
    CHECK(first.bounds == second.bounds);
    CHECK(rv::geometry::hashMesh(first) != rv::geometry::hashMesh(make(0.51f)));

    // Negative sizes use their magnitude: same geometry as the positive size.
    CHECK(rv::geometry::hashMesh(buildBox({-1.0f, 2.0f, -3.0f})) ==
          rv::geometry::hashMesh(buildBox({1.0f, 2.0f, 3.0f})));
}

TEST_CASE("geometry/mesh_builder: invalid input is skipped without NaN", "[geometry][mesh_builder][robustness]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    glm::mat4 broken(1.0f);
    broken[3][0] = inf;

    MeshBuilder builder;
    builder.box({nan, 1.0f, 1.0f})
        .box({1.0f, 1.0f, 1.0f}, broken)
        .box({1.0f, 1.0f, 1.0f}, scale(1.0f, 0.0f, 1.0f))
        .box({1.0f, 1.0f, 1.0f}, kIdentity, BoxUv::WorldMeters, nan)
        .chamferedBox({1.0f, 1.0f, 1.0f}, nan)
        .plane({inf, 1.0f})
        .quad({1.0f, 1.0f}, kIdentity, {0.0f, nan, 1.0f, 1.0f})
        .uvSphere(nan, 8, 4)
        .cylinder(1.0f, inf, 8, true)
        .frustum(1.0f, 0.5f, 1.0f, 8, true, false, broken);
    CHECK(builder.empty());

    // A valid primitive after the skipped ones is still appended.
    builder.box({1.0f, 1.0f, 1.0f});
    const MeshData mesh = std::move(builder).build("survivor");
    CHECK(mesh.vertices.size() == 24);
    CHECK(rv::geometry::validateMesh(mesh).has_value());

    // Huge segment counts are clamped.
    MeshBuilder dense;
    dense.cylinder(1.0f, 1.0f, 1000000, false);
    CHECK(std::move(dense).build("dense").vertices.size() == 2 * (1024 + 1));

    // Appended meshes are checked once placed: a non-finite position or uv, even on an unused vertex, or a
    // placement beyond the float range skips the whole mesh; a non-finite normal is replaced.
    const glm::vec3 n(0.0f, 0.0f, 1.0f);
    MeshData triangle;
    triangle.vertices = {Vertex{{0.0f, 0.0f, 0.0f}, n, {}, {0.0f, 1.0f}},
                         Vertex{{1.0f, 0.0f, 0.0f}, n, {}, {1.0f, 1.0f}},
                         Vertex{{0.0f, 1.0f, 0.0f}, n, {}, {0.0f, 0.0f}}};
    triangle.indices = {0, 1, 2};
    MeshData nanPosition = triangle;
    nanPosition.vertices[1].position.x = nan;
    MeshData infUv = triangle;
    infUv.vertices[2].uv0.y = inf;
    MeshData unusedNan = triangle;
    unusedNan.vertices.push_back(Vertex{{nan, 0.0f, 0.0f}, n, {}, {0.0f, 0.0f}});
    MeshData far = triangle;
    far.vertices[1].position.x = 1e30f;
    MeshBuilder appended;
    appended.append(nanPosition).append(infUv).append(unusedNan).append(far, scale(1e12f, 1e12f, 1e12f));
    CHECK(appended.empty());
    MeshData badNormal = triangle;
    badNormal.vertices[0].normal = glm::vec3(nan);
    appended.append(triangle).append(badNormal);
    const MeshData kept = std::move(appended).build("kept");
    CHECK(kept.vertices.size() == 6);
    CHECK(kept.indices.size() == 6);
    CHECK(rv::geometry::validateMesh(kept).has_value());
}

TEST_CASE("geometry/mesh_builder: torus around +Y, outward normals, closed seams", "[geometry][mesh_builder][torus]") {
    // I6 step 5: the hub's rings (the landmark's handrail and globe, the columns' mouldings).
    MeshBuilder builder;
    builder.torus(0.5f, 0.05f, 32, 8);
    const MeshData ring = std::move(builder).build("torus");
    CHECK(ring.vertices.size() == 33 * 9);
    CHECK(ring.indices.size() == 6 * 32 * 8);
    CHECK(rv::geometry::validateMesh(ring).has_value());
    CHECK(closeTo(ring.bounds.min, glm::vec3(-0.55f, -0.05f, -0.55f), 1e-5f));
    CHECK(closeTo(ring.bounds.max, glm::vec3(0.55f, 0.05f, 0.55f), 1e-5f));
    CHECK(rv::test::nonUnitNormals(ring) == 0);
    CHECK(rv::test::windingMismatches(ring) == 0);
    for (const Vertex& v : ring.vertices) {
        // The normal points from the tube's centre line to the vertex.
        const glm::vec3 axis = glm::normalize(glm::vec3(v.position.x, 0.0f, v.position.z)) * 0.5f;
        CHECK(closeTo(v.normal, (v.position - axis) / 0.05f, 1e-4f));
    }
    // A tube as wide as the circle, a zero radius or a non-finite one appends nothing.
    MeshBuilder empty;
    empty.torus(0.1f, 0.1f, 16, 8).torus(0.0f, 0.01f, 16, 8).torus(0.5f, std::nanf(""), 16, 8);
    CHECK(empty.empty());
}
