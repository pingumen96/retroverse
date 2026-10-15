#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "support/temp_dir.hpp"
#include <rv/assets/glb_writer.hpp>
#include <rv/assets/gltf_loader.hpp>
#include <rv/core/file_io.hpp>
#include <rv/geometry/mesh_builder.hpp>

using Catch::Approx;
using rv::assets::GlbDescription;
using rv::assets::GlbMaterial;
using rv::assets::GlbNode;

namespace {

std::uint32_t u32(const std::vector<std::byte>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

/// One textured quad on a node.
GlbDescription texturedQuad() {
    rv::assets::ImageData texel;
    texel.width = 2;
    texel.height = 1;
    texel.channels = 4;
    texel.pixels = {200, 10, 10, 255, 10, 10, 200, 255};
    GlbDescription description;
    GlbMaterial material;
    material.name = "Label";
    material.baseColorImage = texel;
    description.materials.push_back(material);
    rv::assets::GlbMesh quad = rv::assets::makeQuadMesh("quad", 0.4f, 0.3f);
    quad.material = 0;
    description.meshes.push_back(quad);
    GlbNode node;
    node.name = "Quad";
    node.mesh = 0;
    description.nodes.push_back(node);
    return description;
}

} // namespace

TEST_CASE("assets/glb: the writer produces an aligned two-chunk container", "[assets][glb]") {
    const auto glb = rv::assets::buildGlb(texturedQuad());
    REQUIRE(glb.has_value());
    REQUIRE(glb->size() > 28);
    CHECK(u32(*glb, 0) == 0x46546C67u); // "glTF"
    CHECK(u32(*glb, 4) == 2u);
    CHECK(u32(*glb, 8) == glb->size());
    const std::uint32_t jsonLength = u32(*glb, 12);
    CHECK(u32(*glb, 16) == 0x4E4F534Au); // "JSON"
    CHECK(jsonLength % 4 == 0);
    const std::byte lastJsonByte = (*glb)[20 + jsonLength - 1]; // JSON chunks are padded with spaces
    CHECK((lastJsonByte == std::byte{'}'} || lastJsonByte == std::byte{' '}));
    const std::size_t binHeader = 20 + jsonLength;
    const std::uint32_t binLength = u32(*glb, binHeader);
    CHECK(u32(*glb, binHeader + 4) == 0x004E4942u); // "BIN\0"
    CHECK(binLength % 4 == 0);
    CHECK(binHeader + 8 + binLength == glb->size());

    // Without geometry or images there is no binary chunk.
    GlbDescription empty;
    empty.nodes.push_back(GlbNode{.name = "Lonely"});
    const auto jsonOnly = rv::assets::buildGlb(empty);
    REQUIRE(jsonOnly.has_value());
    CHECK(20 + u32(*jsonOnly, 12) == jsonOnly->size());
}

TEST_CASE("assets/glb: a written file loads back with its geometry and embedded image", "[assets][glb]") {
    rv::test::TempDir dir;
    const auto path = dir.file("nested/quad.glb");
    REQUIRE(rv::assets::writeGlb(path, texturedQuad()).has_value()); // parent directories are created
    const auto model = rv::assets::loadGltf(path);
    REQUIRE(model.has_value());
    CHECK(model->warnings.empty());
    REQUIRE(model->meshes.size() == 1);
    CHECK(model->bounds.size().x == Approx(0.4f));
    CHECK(model->bounds.size().y == Approx(0.3f));
    REQUIRE(model->images.size() == 1);
    CHECK(model->images[0].width == 2);
    CHECK(model->images[0].pixels[0] == 200); // top-left texel first: never flipped
    REQUIRE(model->materials.size() == 1);
    CHECK(model->materials[0].baseColorTexture.imageIndex == std::optional<std::uint32_t>(0));
}

TEST_CASE("assets/glb: meshes built in code keep their attributes", "[assets][glb]") {
    rv::geometry::MeshBuilder builder;
    builder.box(glm::vec3(0.5f, 0.2f, 0.1f));
    const rv::geometry::MeshData box = std::move(builder).build("box");
    const rv::assets::GlbMesh mesh = rv::assets::toGlbMesh(box, 0);
    CHECK(mesh.name == "box");
    CHECK(mesh.material == std::optional<int>(0));
    REQUIRE(mesh.positions.size() == box.vertices.size());
    CHECK(mesh.normals.size() == box.vertices.size());
    CHECK(mesh.tangents.size() == box.vertices.size());
    CHECK(mesh.uvs.size() == box.vertices.size());
    CHECK(mesh.indices == box.indices);

    GlbDescription description;
    description.materials.push_back(GlbMaterial{.name = "Plastic"});
    description.meshes.push_back(mesh);
    description.nodes.push_back(GlbNode{.name = "Box", .mesh = 0});
    const auto glb = rv::assets::buildGlb(description);
    REQUIRE(glb.has_value());
    const auto model = rv::assets::loadGltfFromMemory(*glb, {});
    REQUIRE(model.has_value());
    REQUIRE(model->meshes.size() == 1);
    CHECK(model->meshes[0].vertices.size() == box.vertices.size());
    CHECK(model->meshes[0].triangleCount() == box.triangleCount());
    CHECK(model->meshes[0].vertices[5].tangent == box.vertices[5].tangent); // tangents read back as written
}

TEST_CASE("assets/glb: a base color image that cannot be encoded is an error, not an exception", "[assets][glb]") {
    GlbDescription description = texturedQuad();
    description.materials[0].baseColorImage->pixels.pop_back(); // inconsistent with its size
    const auto glb = rv::assets::buildGlb(description);
    REQUIRE_FALSE(glb.has_value());
    CHECK(glb.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_FALSE(rv::assets::buildGltfWithEmbeddedBuffer(description).has_value());
    CHECK_FALSE(rv::assets::buildGltfWithExternalBuffer(description, "quad.bin").has_value());

    rv::test::TempDir dir;
    const auto written = rv::assets::writeGlb(dir.file("broken.glb"), description);
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().context.find("broken.glb") != std::string::npos);
}

TEST_CASE("assets/glb: names that are not UTF-8 are an error, not an exception", "[assets][glb]") {
    GlbDescription description = texturedQuad();
    description.nodes[0].name = "caf\xE9"; // Latin-1
    const auto glb = rv::assets::buildGlb(description);
    REQUIRE_FALSE(glb.has_value());
    CHECK(glb.error().code == rv::ErrorCode::ParseError);
}
