#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "support/temp_dir.hpp"
#include <rv/assets/glb_writer.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>

using Catch::Approx;
using rv::ErrorCode;
using rv::assets::GlbDescription;
using rv::assets::GlbMaterial;
using rv::assets::GlbMesh;
using rv::assets::GlbNode;
using rv::assets::GltfLoadOptions;
using rv::assets::ModelData;
using rv::geometry::SocketKind;

namespace {

/// The GLB of `description` (the writer's failure fails the test).
std::vector<std::byte> glbBytes(const GlbDescription& description) {
    auto glb = rv::assets::buildGlb(description);
    REQUIRE(glb.has_value());
    return std::move(*glb);
}

/// The .gltf text of `description` with its buffer embedded as a data URI.
std::string embeddedGltf(const GlbDescription& description) {
    auto text = rv::assets::buildGltfWithEmbeddedBuffer(description);
    REQUIRE(text.has_value());
    return std::move(*text);
}

/// The .gltf text of `description` referencing its buffer as `binUri`, and the buffer.
rv::assets::GltfFiles externalGltf(const GlbDescription& description, std::string_view binUri) {
    auto files = rv::assets::buildGltfWithExternalBuffer(description, binUri);
    REQUIRE(files.has_value());
    return std::move(*files);
}

rv::Expected<ModelData> load(const GlbDescription& description, const GltfLoadOptions& options = {}) {
    const std::vector<std::byte> glb = glbBytes(description);
    return rv::assets::loadGltfFromMemory(glb, {}, options);
}

bool hasWarning(const ModelData& model, std::string_view needle) {
    return std::any_of(model.warnings.begin(), model.warnings.end(),
                       [needle](const std::string& warning) { return warning.find(needle) != std::string::npos; });
}

const rv::geometry::Socket* socketById(const ModelData& model, std::string_view id) {
    return rv::geometry::findSocket(model.sockets, id);
}

GlbNode node(std::string name, glm::vec3 translation = glm::vec3(0.0f), std::optional<int> mesh = std::nullopt) {
    GlbNode result;
    result.name = std::move(name);
    result.translation = translation;
    result.mesh = mesh;
    return result;
}

/// `size` x `size` vertices in the XY plane (normals +Z), two triangles per cell.
GlbMesh makeGridMesh(std::string name, int size) {
    GlbMesh mesh;
    mesh.name = std::move(name);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            mesh.positions.emplace_back(0.01f * static_cast<float>(x), 0.01f * static_cast<float>(y), 0.0f);
            mesh.normals.emplace_back(0.0f, 0.0f, 1.0f);
            mesh.uvs.emplace_back(static_cast<float>(x) / static_cast<float>(size - 1),
                                  static_cast<float>(y) / static_cast<float>(size - 1));
        }
    }
    for (int y = 0; y + 1 < size; ++y) {
        for (int x = 0; x + 1 < size; ++x) {
            const auto corner = static_cast<std::uint32_t>(y * size + x);
            const auto row = static_cast<std::uint32_t>(size);
            mesh.indices.insert(mesh.indices.end(), {corner, corner + 1, corner + row + 1});
            mesh.indices.insert(mesh.indices.end(), {corner, corner + row + 1, corner + row});
        }
    }
    return mesh;
}

/// Root > Body, ScreenSurface, Anchor_TV_Stand, Anchor_ScreenLook, PowerButton, SpeakerLeft, ColliderProxy.
GlbDescription makeCrtLikeModel() {
    GlbDescription description;
    description.meshes.push_back(rv::assets::makeQuadMesh("body", 0.71f, 0.61f));
    description.meshes.push_back(rv::assets::makeQuadMesh("screen", 0.48f, 0.36f));
    description.meshes.push_back(rv::assets::makeQuadMesh("collider", 0.71f, 0.61f));
    description.meshes.push_back(rv::assets::makeQuadMesh("button", 0.02f, 0.02f));

    GlbNode root = node("Root");
    root.children = {1, 2, 3, 4, 5, 6, 7};
    description.nodes.push_back(root);
    description.nodes.push_back(node("Body", glm::vec3(0.0f, 0.305f, 0.0f), 0));
    description.nodes.push_back(node("ScreenSurface", glm::vec3(0.0f, 0.33f, 0.285f), 1));
    description.nodes.push_back(node("Anchor_TV_Stand"));
    description.nodes.push_back(node("Anchor_ScreenLook", glm::vec3(0.0f, 0.33f, 1.2f)));
    description.nodes.push_back(node("PowerButton", glm::vec3(0.3f, 0.05f, 0.29f), 3));
    description.nodes.push_back(node("SpeakerLeft", glm::vec3(-0.3f, 0.2f, 0.28f)));
    description.nodes.push_back(node("ColliderProxy", glm::vec3(0.0f, 0.305f, 0.0f), 2));
    return description;
}

} // namespace

TEST_CASE("assets/gltf: nested TRS transforms produce world matrices", "[assets][gltf]") {
    GlbDescription description;
    description.meshes.push_back(rv::assets::makeQuadMesh("quad"));
    GlbNode root = node("Root", glm::vec3(1.0f, 0.0f, 0.0f));
    root.rotation = glm::angleAxis(rv::core::kHalfPi, glm::vec3(0.0f, 1.0f, 0.0f));
    root.scale = glm::vec3(2.0f);
    root.children = {1};
    GlbNode child = node("Child", glm::vec3(0.0f, 0.0f, 1.0f), 0);
    child.children = {2};
    GlbNode leaf = node("Leaf");
    leaf.matrix = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f));
    description.nodes = {root, child, leaf};

    const auto model = load(description);
    REQUIRE(model.has_value());
    REQUIRE(model->nodes.size() == 3);
    CHECK(model->nodes[0].parent == -1);
    CHECK(model->nodes[1].parent == 0);
    CHECK(model->nodes[2].parent == 1);
    CHECK(model->nodes[1].name == "Child");

    const glm::mat4 expectedRoot = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f)) *
                                   glm::rotate(glm::mat4(1.0f), rv::core::kHalfPi, glm::vec3(0.0f, 1.0f, 0.0f)) *
                                   glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
    CHECK(rv::core::nearlyEqual(model->nodes[0].world, expectedRoot, 1e-5f));
    // Child origin: (1, 0, 0) + Ry(90) * (2 * (0, 0, 1)) = (3, 0, 0).
    const glm::vec4 childOrigin = model->nodes[1].world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(childOrigin.x == Approx(3.0f).margin(1e-5));
    CHECK(childOrigin.y == Approx(0.0f).margin(1e-5));
    CHECK(childOrigin.z == Approx(0.0f).margin(1e-5));
    // Matrix node: local is the file's matrix, world goes through both parents (scale 2 applies to 0.5 m).
    CHECK(rv::core::nearlyEqual(model->nodes[2].local, leaf.matrix.value(), 1e-6f));
    const glm::vec4 leafOrigin = model->nodes[2].world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(leafOrigin.x == Approx(3.0f).margin(1e-5));
    CHECK(leafOrigin.y == Approx(1.0f).margin(1e-5));

    REQUIRE(model->drawItems.size() == 1);
    CHECK(model->drawItems[0].node == 1);
    CHECK(model->drawItems[0].meshIndex == 0);
    CHECK(rv::core::nearlyEqual(model->drawItems[0].world, model->nodes[1].world));
    CHECK(model->nodes[1].mesh == std::optional<std::uint32_t>(0));
    CHECK(model->nodes[1].meshCount == 1);
    CHECK_FALSE(model->nodes[0].mesh.has_value());

    // The quad (1 x 1 in XY) is rotated into the YZ plane, scaled by 2 and centered on (3, 0, 0).
    CHECK(model->bounds.min.x == Approx(3.0f).margin(1e-5));
    CHECK(model->bounds.max.x == Approx(3.0f).margin(1e-5));
    CHECK(model->bounds.size().y == Approx(2.0f).margin(1e-5));
    CHECK(model->bounds.size().z == Approx(2.0f).margin(1e-5));

    CHECK(model->stats.nodes == 3);
    CHECK(model->stats.meshes == 1);
    CHECK(model->stats.primitives == 1);
    CHECK(model->stats.triangles == 2);
    CHECK(model->stats.vertices == 4);
    CHECK_FALSE(hasWarning(*model, "non-uniform"));
}

TEST_CASE("assets/gltf: sockets are classified by node name", "[assets][gltf]") {
    GlbDescription description = makeCrtLikeModel();
    description.meshes.push_back(rv::assets::makeQuadMesh("second screen", 0.2f, 0.15f));
    description.nodes[0].children.insert(description.nodes[0].children.end(), {8, 9, 10, 11});
    description.nodes.push_back(node("ScreenSurface_2", glm::vec3(0.5f, 0.3f, 0.0f), 4));
    description.nodes.push_back(node("ScreenSurfaceGlass", glm::vec3(0.0f), 1)); // not a socket name
    description.nodes.push_back(node("Anchor_"));                                // no id
    description.nodes.push_back(node("SpeakerRight", glm::vec3(0.3f, 0.2f, 0.28f)));

    const auto model = load(description);
    REQUIRE(model.has_value());

    const auto* stand = socketById(*model, "TV_Stand");
    REQUIRE(stand != nullptr);
    CHECK(stand->kind == SocketKind::Anchor);
    CHECK(stand->nodeName == "Anchor_TV_Stand");
    CHECK_FALSE(stand->meshIndex.has_value());
    CHECK(stand->bounds.isEmpty());

    const auto* look = socketById(*model, "ScreenLook");
    REQUIRE(look != nullptr);
    CHECK(look->kind == SocketKind::Anchor);
    CHECK(rv::core::nearlyEqual(glm::vec3(look->transform[3]), glm::vec3(0.0f, 0.33f, 1.2f)));

    const auto* screen = socketById(*model, "ScreenSurface");
    REQUIRE(screen != nullptr);
    CHECK(screen->kind == SocketKind::ScreenSurface);
    REQUIRE(screen->meshIndex.has_value());
    CHECK(model->meshes[*screen->meshIndex].name == "screen");
    CHECK(screen->bounds.size().x == Approx(0.48f));
    CHECK(screen->bounds.size().y == Approx(0.36f));
    CHECK(rv::core::nearlyEqual(screen->transform, model->nodes[2].world));

    const auto* second = socketById(*model, "ScreenSurface_2");
    REQUIRE(second != nullptr);
    CHECK(second->kind == SocketKind::ScreenSurface);

    for (const std::string_view other : {"PowerButton", "SpeakerLeft", "SpeakerRight"}) {
        CAPTURE(other);
        const auto* socket = socketById(*model, other);
        REQUIRE(socket != nullptr);
        CHECK(socket->kind == SocketKind::Other);
        CHECK(socket->nodeName == other);
    }
    const auto* collider = socketById(*model, "ColliderProxy");
    REQUIRE(collider != nullptr);
    CHECK(collider->kind == SocketKind::ColliderProxy);
    CHECK(collider->meshIndex.has_value());

    CHECK(socketById(*model, "ScreenSurfaceGlass") == nullptr);
    CHECK(socketById(*model, "Body") == nullptr);
    CHECK(socketById(*model, "Root") == nullptr);
    CHECK(model->sockets.size() == 8);
    CHECK(hasWarning(*model, "Anchor_"));
    // Sockets come in depth-first scene order.
    CHECK(model->sockets.front().nodeName == "ScreenSurface");
}

TEST_CASE("assets/gltf: a ScreenSurface without a mesh is a warning", "[assets][gltf]") {
    GlbDescription description;
    description.nodes = {node("ScreenSurface"), node("ColliderProxyBox")};
    const auto model = load(description);
    REQUIRE(model.has_value());
    REQUIRE(model->sockets.size() == 2);
    CHECK(model->sockets[0].kind == SocketKind::ScreenSurface);
    CHECK_FALSE(model->sockets[0].meshIndex.has_value());
    CHECK(hasWarning(*model, "ScreenSurface node \"ScreenSurface\" has no mesh"));
    CHECK(model->sockets[1].kind == SocketKind::ColliderProxy);
    CHECK(model->sockets[1].id == "ColliderProxyBox");
    CHECK(model->drawItems.empty());
    CHECK(model->bounds.isEmpty());
}

TEST_CASE("assets/gltf: ColliderProxy nodes are excluded from drawing by default", "[assets][gltf]") {
    GlbDescription description = makeCrtLikeModel();
    description.nodes[7].translation = glm::vec3(0.0f, 0.305f, -1.0f); // collider far behind the body

    const auto model = load(description);
    REQUIRE(model.has_value());
    const auto drawsNode = [](const ModelData& data, std::string_view name) {
        return std::any_of(data.drawItems.begin(), data.drawItems.end(),
                           [&](const rv::assets::ModelDrawItem& item) { return data.nodes[item.node].name == name; });
    };
    CHECK(drawsNode(*model, "Body"));
    CHECK(drawsNode(*model, "ScreenSurface"));
    CHECK(drawsNode(*model, "PowerButton"));
    CHECK_FALSE(drawsNode(*model, "ColliderProxy"));
    CHECK(model->drawItems.size() == 3);
    CHECK(model->bounds.min.z == Approx(0.0f).margin(1e-5)); // the collider does not count
    CHECK(model->meshes.size() == 4);                        // its mesh is still loaded (collision)

    GltfLoadOptions options;
    options.excludeColliderProxies = false;
    const auto withCollider = load(description, options);
    REQUIRE(withCollider.has_value());
    CHECK(drawsNode(*withCollider, "ColliderProxy"));
    CHECK(withCollider->drawItems.size() == 4);
    CHECK(withCollider->bounds.min.z == Approx(-1.0f).margin(1e-5));
}

TEST_CASE("assets/gltf: embedded PNG textures are decoded and referenced", "[assets][gltf]") {
    rv::assets::ImageData texture;
    texture.width = 2;
    texture.height = 2;
    texture.channels = 4;
    texture.pixels = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 128};

    GlbDescription description;
    GlbMaterial material;
    material.name = "Label";
    material.baseColorFactor = glm::vec4(0.5f, 0.25f, 1.0f, 1.0f);
    material.metallicFactor = 0.0f;
    material.roughnessFactor = 0.6f;
    material.baseColorImage = texture;
    description.materials.push_back(material);
    GlbMesh quad = rv::assets::makeQuadMesh("label");
    quad.material = 0;
    description.meshes.push_back(quad);
    description.nodes = {node("Label", glm::vec3(0.0f), 0)};

    const auto model = load(description);
    REQUIRE(model.has_value());
    REQUIRE(model->images.size() == 1);
    CHECK(model->images[0].width == 2);
    CHECK(model->images[0].height == 2);
    CHECK(model->images[0].channels == 4);
    CHECK(model->images[0].pixels == texture.pixels); // top-left texel first: never flipped
    REQUIRE(model->materials.size() == 1);
    const rv::assets::MaterialDesc& desc = model->materials[0];
    CHECK(desc.name == "Label");
    CHECK(desc.baseColorFactor == glm::vec4(0.5f, 0.25f, 1.0f, 1.0f));
    CHECK(desc.roughnessFactor == Approx(0.6f));
    CHECK(desc.metallicFactor == Approx(0.0f));
    CHECK(desc.baseColorTexture.imageIndex == std::optional<std::uint32_t>(0));
    CHECK(desc.baseColorTexture.texCoord == 0);
    CHECK(desc.normalTexture.empty());
    REQUIRE(model->drawItems.size() == 1);
    CHECK(model->drawItems[0].material == std::optional<std::uint32_t>(0));
    CHECK(model->stats.images == 1);
    CHECK(model->stats.materials == 1);
    // glTF UVs are kept as authored: (0, 0) at the top-left corner of the quad.
    CHECK(model->meshes[0].vertices[0].uv0 == glm::vec2(0.0f, 0.0f));
    CHECK(model->meshes[0].vertices[0].position.y > 0.0f);

    GltfLoadOptions noImages;
    noImages.loadImages = false;
    const auto withoutImages = load(description, noImages);
    REQUIRE(withoutImages.has_value());
    CHECK(withoutImages->images.empty());
    CHECK(withoutImages->materials[0].baseColorTexture.empty());
}

TEST_CASE("assets/gltf: a corrupt embedded image is dropped with a warning", "[assets][gltf]") {
    GlbDescription description;
    GlbMaterial material;
    material.name = "Broken";
    material.baseColorImageBytes = {std::byte{0x89}, std::byte{'P'},  std::byte{'N'},  std::byte{'G'},
                                    std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A},
                                    std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    description.materials.push_back(material);
    GlbMesh quad = rv::assets::makeQuadMesh("quad");
    quad.material = 0;
    description.meshes.push_back(quad);
    description.nodes = {node("Quad", glm::vec3(0.0f), 0)};

    const auto model = load(description);
    REQUIRE(model.has_value());
    CHECK(model->images.empty());
    CHECK(model->materials[0].baseColorTexture.empty());
    CHECK(hasWarning(*model, "image 0"));
    CHECK(model->drawItems.size() == 1);
}

TEST_CASE("assets/gltf: tangents are generated with unit length or taken from the file", "[assets][gltf]") {
    GlbDescription description;
    description.meshes.push_back(rv::assets::makeQuadMesh("generated"));
    GlbMesh authored = rv::assets::makeQuadMesh("authored");
    authored.tangents.assign(4, glm::vec4(0.0f, 2.0f, 0.0f, -3.0f)); // not normalized on purpose
    description.meshes.push_back(authored);
    description.nodes = {node("A", glm::vec3(0.0f), 0), node("B", glm::vec3(2.0f, 0.0f, 0.0f), 1)};

    const auto model = load(description);
    REQUIRE(model.has_value());
    REQUIRE(model->meshes.size() == 2);
    for (const rv::geometry::Vertex& vertex : model->meshes[0].vertices) {
        const glm::vec3 tangent(vertex.tangent);
        CHECK(glm::length(tangent) == Approx(1.0f).margin(1e-5));
        CHECK(glm::dot(tangent, vertex.normal) == Approx(0.0f).margin(1e-5));
        CHECK(std::abs(vertex.tangent.w) == Approx(1.0f));
        CHECK(tangent.x == Approx(1.0f).margin(1e-5)); // u grows towards +X
    }
    for (const rv::geometry::Vertex& vertex : model->meshes[1].vertices) {
        CHECK(vertex.tangent == glm::vec4(0.0f, 1.0f, 0.0f, -1.0f));
    }

    GltfLoadOptions options;
    options.generateTangents = false;
    const auto untouched = load(description, options);
    REQUIRE(untouched.has_value());
    CHECK(untouched->meshes[0].vertices[0].tangent == rv::geometry::Vertex{}.tangent);
}

TEST_CASE("assets/gltf: missing normals become flat normals with a warning", "[assets][gltf]") {
    GlbDescription description;
    GlbMesh triangles;
    triangles.name = "flat";
    // Two triangles sharing an edge (vertices 1 and 2) but not coplanar: each face gets its own normal.
    triangles.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    triangles.indices = {0, 1, 2, 1, 2, 3};
    description.meshes.push_back(triangles);
    description.nodes = {node("Flat", glm::vec3(0.0f), 0)};

    const auto model = load(description);
    REQUIRE(model.has_value());
    CHECK(hasWarning(*model, "no normals"));
    const auto& mesh = model->meshes[0];
    REQUIRE(mesh.vertices.size() == 6);
    for (int v = 0; v < 3; ++v) {
        CHECK(rv::core::nearlyEqual(mesh.vertices[static_cast<std::size_t>(v)].normal, glm::vec3(0.0f, 0.0f, 1.0f)));
    }
    const glm::vec3 secondNormal = glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f));
    for (int v = 3; v < 6; ++v) {
        CHECK(rv::core::nearlyEqual(mesh.vertices[static_cast<std::size_t>(v)].normal, secondNormal, 1e-5f));
    }
    CHECK(rv::geometry::validateMesh(mesh).has_value());
}

TEST_CASE("assets/gltf: missing indices are generated", "[assets][gltf]") {
    GlbDescription description;
    GlbMesh soup = rv::assets::makeQuadMesh("soup");
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> uvs;
    for (const std::uint32_t index : soup.indices) {
        positions.push_back(soup.positions[index]);
        normals.push_back(soup.normals[index]);
        uvs.push_back(soup.uvs[index]);
    }
    soup.positions = positions;
    soup.normals = normals;
    soup.uvs = uvs;
    soup.indices.clear();
    description.meshes.push_back(soup);
    description.nodes = {node("Soup", glm::vec3(0.0f), 0)};

    const auto model = load(description);
    REQUIRE(model.has_value());
    CHECK(model->meshes[0].indices == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5});
    CHECK(model->meshes[0].triangleCount() == 2);
}

TEST_CASE("assets/gltf: triangle strips and fans become lists; points are skipped", "[assets][gltf]") {
    GlbDescription description;
    GlbMesh strip = rv::assets::makeQuadMesh("strip");
    strip.indices = {0, 1, 3, 2}; // TL, BL, TR, BR
    strip.mode = 5;
    GlbMesh fan = rv::assets::makeQuadMesh("fan");
    fan.indices = {0, 1, 2, 3};
    fan.mode = 6;
    GlbMesh points = rv::assets::makeQuadMesh("points");
    points.mode = 0;
    description.meshes = {strip, fan, points};
    description.nodes = {node("Strip", glm::vec3(0.0f), 0), node("Fan", glm::vec3(2.0f, 0.0f, 0.0f), 1),
                         node("Points", glm::vec3(4.0f, 0.0f, 0.0f), 2)};

    const auto model = load(description);
    REQUIRE(model.has_value());
    REQUIRE(model->meshes.size() == 2);
    CHECK(model->meshes[0].indices == std::vector<std::uint32_t>{0, 1, 3, 1, 2, 3});
    CHECK(model->meshes[1].indices == std::vector<std::uint32_t>{1, 2, 0, 2, 3, 0});
    for (const auto& mesh : model->meshes) {
        // Every triangle keeps the counter-clockwise winding seen from +Z.
        for (std::size_t t = 0; t < mesh.indices.size(); t += 3) {
            const glm::vec3 a = mesh.vertices[mesh.indices[t]].position;
            const glm::vec3 b = mesh.vertices[mesh.indices[t + 1]].position;
            const glm::vec3 c = mesh.vertices[mesh.indices[t + 2]].position;
            CHECK(glm::cross(b - a, c - a).z > 0.0f);
        }
    }
    CHECK(hasWarning(*model, "POINTS"));
    CHECK_FALSE(model->nodes[2].mesh.has_value());
    CHECK(model->drawItems.size() == 2);
}

TEST_CASE("assets/gltf: truncated or invalid containers are errors", "[assets][gltf]") {
    GlbDescription description = makeCrtLikeModel();
    const std::vector<std::byte> glb = glbBytes(description);

    const std::vector<std::byte> truncated(glb.begin(), glb.end() - 40);
    const auto result = rv::assets::loadGltfFromMemory(truncated, {});
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code == ErrorCode::ParseError);
    CHECK(result.error().message.find("truncated") != std::string::npos);

    const std::vector<std::byte> headerOnly(glb.begin(), glb.begin() + 10);
    CHECK(rv::assets::loadGltfFromMemory(headerOnly, {}).error().code == ErrorCode::ParseError);

    std::vector<std::byte> wrongVersion = glb;
    wrongVersion[4] = std::byte{1};
    CHECK(rv::assets::loadGltfFromMemory(wrongVersion, {}).error().code == ErrorCode::Unsupported);

    const std::string notGltf = "this is not a model";
    CHECK(rv::assets::loadGltfFromMemory(std::as_bytes(std::span(notGltf)), {}).error().code == ErrorCode::ParseError);
    const std::string brokenJson = "{\"asset\": {\"version\": \"2.0\"}, \"nodes\": [";
    CHECK(rv::assets::loadGltfFromMemory(std::as_bytes(std::span(brokenJson)), {}).error().code ==
          ErrorCode::ParseError);
    CHECK_FALSE(rv::assets::loadGltfFromMemory({}, {}).has_value());

    GltfLoadOptions tiny;
    tiny.maxFileBytes = 64;
    CHECK(rv::assets::loadGltfFromMemory(glb, {}, tiny).error().code == ErrorCode::OutOfRange);
}

TEST_CASE("assets/gltf: unsupported required extensions are errors naming the extension", "[assets][gltf]") {
    for (const std::string extension : {"KHR_draco_mesh_compression", "EXT_made_up_extension", "KHR_materials_unlit"}) {
        CAPTURE(extension);
        GlbDescription description = makeCrtLikeModel();
        description.extensionsUsed = {extension};
        description.extensionsRequired = {extension};
        const auto model = load(description);
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().code == ErrorCode::Unsupported);
        CHECK(model.error().message.find(extension) != std::string::npos);
    }

    GlbDescription supported = makeCrtLikeModel();
    supported.extensionsUsed = {"KHR_texture_transform", "KHR_materials_emissive_strength"};
    supported.extensionsRequired = {"KHR_texture_transform", "KHR_materials_emissive_strength"};
    const auto model = load(supported);
    REQUIRE(model.has_value());
    CHECK(model->stats.extensionsUsed ==
          std::vector<std::string>{"KHR_texture_transform", "KHR_materials_emissive_strength"});
}

TEST_CASE("assets/gltf: material extensions are mapped", "[assets][gltf]") {
    rv::assets::ImageData texel;
    texel.width = 1;
    texel.height = 1;
    texel.channels = 3;
    texel.pixels = {200, 100, 50};

    GlbDescription description;
    GlbMaterial glowing;
    glowing.name = "Glowing";
    glowing.baseColorImage = texel;
    GlbMaterial flat;
    flat.name = "Flat";
    description.materials = {glowing, flat};
    GlbMesh quad = rv::assets::makeQuadMesh("quad");
    quad.material = 0;
    GlbMesh second = rv::assets::makeQuadMesh("second");
    second.material = 1;
    description.meshes = {quad, second};
    description.nodes = {node("Quad", glm::vec3(0.0f), 0), node("Second", glm::vec3(2.0f, 0.0f, 0.0f), 1)};
    description.extensionsUsed = {"KHR_materials_emissive_strength", "KHR_texture_transform", "KHR_materials_unlit"};
    description.patchJson = [](nlohmann::json& json) {
        nlohmann::json& material = json["materials"][0];
        material["emissiveFactor"] = {1.0, 0.5, 0.25};
        material["extensions"]["KHR_materials_emissive_strength"] = {{"emissiveStrength", 4.0}};
        material["alphaMode"] = "MASK";
        material["alphaCutoff"] = 0.25;
        material["doubleSided"] = true;
        material["pbrMetallicRoughness"]["baseColorTexture"]["extensions"]["KHR_texture_transform"] = {
            {"offset", {0.5, 0.0}}, {"scale", {2.0, 1.0}}};
        json["materials"][1]["extensions"]["KHR_materials_unlit"] = nlohmann::json::object();
        json["materials"][1]["alphaMode"] = "BLEND";
    };

    const auto model = load(description);
    REQUIRE(model.has_value());
    const rv::assets::MaterialDesc& material = model->materials[0];
    CHECK(material.emissiveFactor == glm::vec3(1.0f, 0.5f, 0.25f));
    CHECK(material.emissiveStrength == Approx(4.0f));
    CHECK(material.alphaMode == rv::assets::AlphaMode::Mask);
    CHECK(material.alphaCutoff == Approx(0.25f));
    CHECK(material.doubleSided);
    CHECK(material.shading == rv::assets::ShadingModel::Pbr);
    CHECK(model->materials[1].shading == rv::assets::ShadingModel::Unlit);
    CHECK(model->materials[1].alphaMode == rv::assets::AlphaMode::Blend);

    // KHR_texture_transform is baked into TEXCOORD_0: uv' = offset + scale * uv.
    const auto& vertices = model->meshes[0].vertices;
    CHECK(vertices[0].uv0.x == Approx(0.5f)); // (0, 0) -> (0.5, 0)
    CHECK(vertices[0].uv0.y == Approx(0.0f));
    CHECK(vertices[2].uv0.x == Approx(2.5f)); // (1, 1) -> (2.5, 1)
    CHECK(vertices[2].uv0.y == Approx(1.0f));
    CHECK(model->meshes[1].vertices[2].uv0 == glm::vec2(1.0f, 1.0f)); // other material untouched
    CHECK_FALSE(hasWarning(*model, "KHR_texture_transform"));
}

TEST_CASE("assets/gltf: texture transform rotation and conflicting transforms", "[assets][gltf]") {
    rv::assets::ImageData texel;
    texel.width = 1;
    texel.height = 1;
    texel.channels = 4;
    texel.pixels = {10, 20, 30, 255};

    GlbDescription description;
    GlbMaterial rotated;
    rotated.name = "Rotated";
    rotated.baseColorImage = texel;
    description.materials = {rotated};
    GlbMesh quad = rv::assets::makeQuadMesh("quad");
    quad.material = 0;
    description.meshes = {quad};
    description.nodes = {node("Quad", glm::vec3(0.0f), 0)};
    description.extensionsUsed = {"KHR_texture_transform"};
    description.patchJson = [](nlohmann::json& json) {
        nlohmann::json& material = json["materials"][0];
        material["pbrMetallicRoughness"]["baseColorTexture"]["extensions"]["KHR_texture_transform"] = {
            {"rotation", rv::core::kHalfPi}};
        // The normal map has no transform: the base color one wins, with a warning.
        material["normalTexture"] = {{"index", 0}};
    };

    const auto model = load(description);
    REQUIRE(model.has_value());
    // uv' = R(r) * uv with R(r) * (u, v) = (cos r u + sin r v, -sin r u + cos r v): (1, 0) -> (0, -1).
    const glm::vec2 topRight = model->meshes[0].vertices[3].uv0;
    CHECK(topRight.x == Approx(0.0f).margin(1e-6));
    CHECK(topRight.y == Approx(-1.0f));
    const glm::vec2 bottomLeft = model->meshes[0].vertices[1].uv0; // (0, 1) -> (1, 0)
    CHECK(bottomLeft.x == Approx(1.0f));
    CHECK(bottomLeft.y == Approx(0.0f).margin(1e-6));
    CHECK(hasWarning(*model, "different KHR_texture_transform values"));
    CHECK(model->materials[0].normalTexture.imageIndex == std::optional<std::uint32_t>(0));
}

TEST_CASE("assets/gltf: scale diagnostics", "[assets][gltf]") {
    SECTION("non-uniform scale on a node with a mesh") {
        GlbDescription description;
        description.meshes.push_back(rv::assets::makeQuadMesh("quad"));
        GlbNode stretched = node("Stretched", glm::vec3(0.0f), 0);
        stretched.scale = glm::vec3(1.0f, 2.0f, 1.0f);
        description.nodes = {stretched};
        const auto model = load(description);
        REQUIRE(model.has_value());
        CHECK(hasWarning(*model, "Stretched"));
        CHECK(hasWarning(*model, "non-uniform"));
    }
    SECTION("mirroring scale") {
        GlbDescription description;
        description.meshes.push_back(rv::assets::makeQuadMesh("quad"));
        GlbNode mirrored = node("Mirrored", glm::vec3(0.0f), 0);
        mirrored.scale = glm::vec3(-1.0f, 1.0f, 1.0f);
        description.nodes = {mirrored};
        const auto model = load(description);
        REQUIRE(model.has_value());
        CHECK(hasWarning(*model, "mirroring"));
    }
    SECTION("uniform scale is fine") {
        GlbDescription description;
        description.meshes.push_back(rv::assets::makeQuadMesh("quad", 0.5f, 0.5f));
        GlbNode scaled = node("Scaled", glm::vec3(0.0f), 0);
        scaled.scale = glm::vec3(1.5f);
        description.nodes = {scaled};
        const auto model = load(description);
        REQUIRE(model.has_value());
        CHECK(model->warnings.empty());
    }
    SECTION("bounds larger than 20 m: probably centimeters") {
        GlbDescription description;
        description.meshes.push_back(rv::assets::makeQuadMesh("crt in centimeters", 71.0f, 61.0f));
        description.nodes = {node("Crt", glm::vec3(0.0f), 0)};
        const auto model = load(description);
        REQUIRE(model.has_value());
        CHECK(hasWarning(*model, "larger than 20 m"));
    }
    SECTION("bounds smaller than 5 mm") {
        GlbDescription description;
        description.meshes.push_back(rv::assets::makeQuadMesh("speck", 0.001f, 0.001f));
        description.nodes = {node("Speck", glm::vec3(0.0f), 0)};
        const auto model = load(description);
        REQUIRE(model.has_value());
        CHECK(hasWarning(*model, "smaller than 5 mm"));
    }
}

TEST_CASE("assets/gltf: invalid vertex data and references are errors", "[assets][gltf]") {
    SECTION("NaN position") {
        GlbDescription description;
        GlbMesh quad = rv::assets::makeQuadMesh("quad");
        quad.positions[2].x = std::numeric_limits<float>::quiet_NaN();
        description.meshes.push_back(quad);
        description.nodes = {node("Quad", glm::vec3(0.0f), 0)};
        const auto model = load(description);
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().message.find("NaN") != std::string::npos);
    }
    SECTION("infinite normal") {
        GlbDescription description;
        GlbMesh quad = rv::assets::makeQuadMesh("quad");
        quad.normals[0].z = std::numeric_limits<float>::infinity();
        description.meshes.push_back(quad);
        description.nodes = {node("Quad", glm::vec3(0.0f), 0)};
        CHECK_FALSE(load(description).has_value());
    }
    SECTION("index out of range") {
        GlbDescription description;
        GlbMesh quad = rv::assets::makeQuadMesh("quad");
        quad.indices[5] = 7;
        description.meshes.push_back(quad);
        description.nodes = {node("Quad", glm::vec3(0.0f), 0)};
        const auto model = load(description);
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().message.find("index 7") != std::string::npos);
    }
    SECTION("accessor reading past its buffer view") {
        GlbDescription description;
        description.meshes.push_back(rv::assets::makeQuadMesh("quad"));
        description.nodes = {node("Quad", glm::vec3(0.0f), 0)};
        description.patchJson = [](nlohmann::json& json) {
            json["accessors"][0]["count"] = 400;
        };
        const auto model = load(description);
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().code == ErrorCode::ParseError);
    }
    SECTION("node hierarchy cycle") {
        GlbDescription description;
        GlbNode a = node("A");
        a.children = {1};
        GlbNode b = node("B");
        b.children = {0};
        description.nodes = {a, b};
        description.sceneRoots = {};
        description.patchJson = [](nlohmann::json& json) {
            json.erase("scenes");
            json.erase("scene");
        };
        const auto model = load(description);
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().code == ErrorCode::ParseError);
    }
    SECTION("node hierarchy cycle next to a valid root") {
        GlbDescription description;
        GlbNode b = node("B");
        b.children = {2};
        GlbNode c = node("C");
        c.children = {1};
        description.nodes = {node("Root"), b, c};
        const auto model = load(description);
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().code == ErrorCode::ParseError);
        CHECK(model.error().message.find("cycle") != std::string::npos);
    }
    SECTION("node with two parents") {
        GlbDescription description;
        GlbNode a = node("A");
        a.children = {2};
        GlbNode b = node("B");
        b.children = {2};
        description.nodes = {a, b, node("Shared")};
        const auto model = load(description);
        REQUIRE_FALSE(model.has_value());
        CHECK(model.error().message.find("more than one parent") != std::string::npos);
    }
}

TEST_CASE("assets/gltf: the default scene selects what is drawn", "[assets][gltf]") {
    GlbDescription description;
    description.meshes.push_back(rv::assets::makeQuadMesh("quad"));
    description.nodes = {node("InSceneZero", glm::vec3(0.0f), 0), node("InSceneOne", glm::vec3(5.0f, 0.0f, 0.0f), 0)};
    description.patchJson = [](nlohmann::json& json) {
        json["scenes"] = nlohmann::json::array({{{"nodes", {0}}}, {{"nodes", {1}}}});
        json["scene"] = 1;
    };
    const auto model = load(description);
    REQUIRE(model.has_value());
    REQUIRE(model->drawItems.size() == 1);
    CHECK(model->nodes[model->drawItems[0].node].name == "InSceneOne");
    CHECK(model->bounds.center().x == Approx(5.0f));
    CHECK(model->nodes.size() == 2); // every node is reported, only the active scene is drawn

    GlbDescription noScene = description;
    noScene.patchJson = [](nlohmann::json& json) {
        json.erase("scenes");
        json.erase("scene");
    };
    const auto rootsOnly = load(noScene);
    REQUIRE(rootsOnly.has_value());
    CHECK(rootsOnly->drawItems.size() == 2);
}

TEST_CASE("assets/gltf: .gltf files with base64 and external buffers", "[assets][gltf]") {
    const GlbDescription description = makeCrtLikeModel();
    rv::test::TempDir dir;

    const std::string embedded = embeddedGltf(description);
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("embedded.gltf"), embedded).has_value());
    const auto fromEmbedded = rv::assets::loadGltf(dir.file("embedded.gltf"));
    REQUIRE(fromEmbedded.has_value());
    CHECK(fromEmbedded->sockets.size() == 6);

    // Unicode directory and file names (UTF-8 literals: the sources are compiled with /utf-8).
    const rv::assets::GltfFiles files = externalGltf(description, "crt_è.bin");
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("modèle/crt.gltf"), files.json).has_value());
    REQUIRE(rv::core::writeFileAtomic(dir.file("modèle/crt_è.bin"), files.bin).has_value());
    const auto fromExternal = rv::assets::loadGltf(dir.file("modèle/crt.gltf"));
    REQUIRE(fromExternal.has_value());
    CHECK(fromExternal->drawItems.size() == fromEmbedded->drawItems.size());
    CHECK(fromExternal->meshes.size() == 4);
    CHECK(rv::geometry::hashMesh(fromExternal->meshes[1]) == rv::geometry::hashMesh(fromEmbedded->meshes[1]));

    // The same file loaded from memory without a base directory cannot reach the external buffer.
    const auto withoutBase = rv::assets::loadGltfFromMemory(std::as_bytes(std::span(files.json)), {});
    REQUIRE_FALSE(withoutBase.has_value());
    CHECK(withoutBase.error().code == ErrorCode::NotFound);

    // External references may not leave the model directory.
    const rv::assets::GltfFiles escaping = externalGltf(description, "../outside.bin");
    REQUIRE(rv::core::writeFileAtomic(dir.file("outside.bin"), escaping.bin).has_value());
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("nested/escape.gltf"), escaping.json).has_value());
    const auto escaped = rv::assets::loadGltf(dir.file("nested/escape.gltf"));
    REQUIRE_FALSE(escaped.has_value());
    CHECK(escaped.error().code == ErrorCode::InvalidArgument);

    const auto missingBin = rv::assets::loadGltf(dir.file("missing/crt.gltf"));
    CHECK(missingBin.error().code == ErrorCode::NotFound);
}

TEST_CASE("assets/gltf: unreadable external image paths drop the texture, not the model", "[assets][gltf]") {
    rv::assets::ImageData texel;
    texel.width = 1;
    texel.height = 1;
    texel.channels = 4;
    texel.pixels = {10, 20, 30, 255};
    GlbDescription description;
    GlbMaterial material;
    material.name = "Cover";
    material.baseColorImage = texel;
    description.materials = {material};
    GlbMesh quad = rv::assets::makeQuadMesh("quad");
    quad.material = 0;
    description.meshes = {quad};
    description.nodes = {node("Quad", glm::vec3(0.0f), 0)};
    rv::test::TempDir dir;

    // The URI percent-decodes to Latin-1 bytes, which are not UTF-8.
    description.patchJson = [](nlohmann::json& json) {
        json["images"][0] = {{"uri", "covers/caf%E9.png"}};
    };
    const std::string latin1 = embeddedGltf(description);
    const auto model = rv::assets::loadGltfFromMemory(std::as_bytes(std::span(latin1)), dir.path());
    REQUIRE(model.has_value());
    CHECK(model->images.empty());
    CHECK(model->materials[0].baseColorTexture.empty());
    CHECK(hasWarning(*model, "image 0"));
    CHECK(hasWarning(*model, "UTF-8"));
    CHECK(model->drawItems.size() == 1);

    // A model directory without a UTF-8 form (unpaired UTF-16 surrogate) cannot be read from either.
    description.patchJson = [](nlohmann::json& json) {
        json["images"][0] = {{"uri", "cover.png"}};
    };
    const std::string plain = embeddedGltf(description);
    const auto odd =
        rv::assets::loadGltfFromMemory(std::as_bytes(std::span(plain)), dir.path() / std::wstring(L"bad\xD800-dir"));
    REQUIRE(odd.has_value());
    CHECK(odd->images.empty());
    CHECK(hasWarning(*odd, "image 0"));

    // Buffers are required, so a buffer path that is not UTF-8 fails the model with a clear code.
    const rv::assets::GltfFiles external = externalGltf(description, "caf%E9.bin");
    const auto noBuffer = rv::assets::loadGltfFromMemory(std::as_bytes(std::span(external.json)), dir.path());
    REQUIRE_FALSE(noBuffer.has_value());
    CHECK(noBuffer.error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("assets/gltf: decoded geometry is bounded by the file size limit", "[assets][gltf]") {
    GltfLoadOptions small;
    small.maxFileBytes = 64 * 1024; // at most 4096 vertices and 16384 indices for the whole model

    SECTION("primitives sharing accessors") {
        GlbDescription description;
        description.meshes.push_back(makeGridMesh("grid", 20)); // 400 vertices, 2166 indices
        description.nodes = {node("Grid", glm::vec3(0.0f), 0)};
        const auto single = load(description, small);
        REQUIRE(single.has_value());
        CHECK(single->stats.vertices == 400);

        // The same primitive ten more times: a few more bytes of JSON, eleven copies of the geometry.
        description.patchJson = [](nlohmann::json& json) {
            nlohmann::json& primitives = json["meshes"][0]["primitives"];
            const nlohmann::json primitive = primitives[0];
            for (int i = 0; i < 10; ++i) {
                primitives.push_back(primitive);
            }
        };
        const std::vector<std::byte> glb = glbBytes(description);
        REQUIRE(glb.size() < small.maxFileBytes);
        const auto shared = rv::assets::loadGltfFromMemory(glb, {}, small);
        REQUIRE_FALSE(shared.has_value());
        CHECK(shared.error().code == ErrorCode::OutOfRange);
        CHECK(load(description).has_value()); // within the default limit
    }
    SECTION("flat normals expanding every index into a vertex") {
        const auto triangleSoup = [](std::size_t triangles) {
            GlbDescription description;
            GlbMesh mesh;
            mesh.name = "soup";
            mesh.positions = {{0.0f, 0.0f, 0.0f}, {0.1f, 0.0f, 0.0f}, {0.0f, 0.1f, 0.0f}};
            for (std::size_t t = 0; t < triangles; ++t) {
                mesh.indices.insert(mesh.indices.end(), {0u, 1u, 2u});
            }
            description.meshes.push_back(mesh);
            description.nodes = {node("Soup", glm::vec3(0.0f), 0)};
            return description;
        };
        const auto fits = load(triangleSoup(1000), small);
        REQUIRE(fits.has_value());
        CHECK(fits->meshes[0].vertices.size() == 3000);

        const auto expanded = load(triangleSoup(2000), small); // 6000 vertices from 3 positions
        REQUIRE_FALSE(expanded.has_value());
        CHECK(expanded.error().code == ErrorCode::OutOfRange);
    }
}

TEST_CASE("assets/gltf: deep chains, many roots and many sockets", "[assets][gltf]") {
    // Hierarchy, scene-root and socket checks run in linear time (quadratic versions took seconds here).
    constexpr int kDepth = 5000;
    constexpr int kRoots = 5000;
    GlbDescription description;
    for (int i = 0; i < kDepth; ++i) {
        GlbNode link = node("SpeakerChain_" + std::to_string(i), glm::vec3(0.0f, 1.0f, 0.0f));
        if (i + 1 < kDepth) {
            link.children = {i + 1};
        }
        description.nodes.push_back(std::move(link));
    }
    for (int i = 0; i < kRoots; ++i) {
        description.nodes.push_back(node("SpeakerRoot_" + std::to_string(i)));
    }
    const auto model = load(description);
    REQUIRE(model.has_value());
    REQUIRE(model->nodes.size() == static_cast<std::size_t>(kDepth + kRoots));
    CHECK(model->nodes[kDepth - 1].parent == kDepth - 2);
    CHECK(model->nodes[kDepth - 1].world[3].y == static_cast<float>(kDepth)); // integer sums are exact
    CHECK(model->sockets.size() == static_cast<std::size_t>(kDepth + kRoots));
    CHECK_FALSE(hasWarning(*model, "duplicate"));
}

TEST_CASE("assets/gltf: a GLB written to disk loads through loadGltf", "[assets][gltf]") {
    rv::test::TempDir dir;
    const std::vector<std::byte> glb = glbBytes(makeCrtLikeModel());
    REQUIRE(rv::core::writeFileAtomic(dir.file("crt.glb"), glb).has_value());
    const auto model = rv::assets::loadGltf(dir.file("crt.glb"));
    REQUIRE(model.has_value());
    CHECK(model->sockets.size() == 6);
    CHECK(model->warnings.empty());
    // Pivot on the floor (Body spans y in [0, 0.61]) and the front faces +Z (ADR-0002).
    CHECK(model->bounds.min.y == Approx(0.0f).margin(1e-5));
    CHECK(model->bounds.max.y == Approx(0.61f).margin(1e-5));
    CHECK(model->bounds.max.z == Approx(0.29f).margin(1e-5));
}
