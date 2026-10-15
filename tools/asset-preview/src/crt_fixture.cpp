#include <rv/preview/crt_fixture.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/math.hpp>
#include <rv/geometry/mesh_builder.hpp>

namespace rv::preview {
namespace {

/// Depth of the body: the screen sits 1 cm in front of its front face, at the front of the fixture's bounds.
constexpr float kBodyDepth = 0.55f;
constexpr float kScreenWidth = 0.48f;
constexpr float kScreenHeight = 0.36f;
constexpr float kScreenCenterY = 0.33f;
/// Where a viewer stands to look at the screen, in front of it.
constexpr float kLookDistance = 0.9f;
constexpr int kTextureSize = 32;

/// The body's box in model space: kCrtFixtureSize wide and tall, kBodyDepth deep, its back at the bounds' back.
geometry::MeshData bodyBox(std::string name) {
    geometry::MeshBuilder builder;
    const glm::vec3 size(kCrtFixtureSize.x, kCrtFixtureSize.y, kBodyDepth);
    const glm::vec3 center(0.0f, 0.5f * kCrtFixtureSize.y, -0.5f * kCrtFixtureSize.z + 0.5f * kBodyDepth);
    builder.box(size, glm::translate(glm::mat4(1.0f), center));
    return std::move(builder).build(std::move(name));
}

/// A warm dark plastic with a fine vertical grain: 32 x 32 RGBA, deterministic.
assets::ImageData plasticTexture() {
    assets::ImageData image;
    image.width = kTextureSize;
    image.height = kTextureSize;
    image.channels = 4;
    image.pixels.reserve(static_cast<std::size_t>(kTextureSize) * kTextureSize * 4);
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            // Integer hash of the column: lines of slightly different brightness, a few percent apart.
            std::uint32_t hash = static_cast<std::uint32_t>(x) * 2654435761u;
            hash ^= hash >> 15;
            const int grain = static_cast<int>(hash % 9u) - 4;
            const int shade = (y % 8 == 0) ? -3 : 0;
            image.pixels.push_back(static_cast<std::uint8_t>(64 + grain + shade));
            image.pixels.push_back(static_cast<std::uint8_t>(58 + grain + shade));
            image.pixels.push_back(static_cast<std::uint8_t>(52 + grain + shade));
            image.pixels.push_back(255);
        }
    }
    return image;
}

assets::GlbNode node(std::string name, glm::vec3 translation = glm::vec3(0.0f), std::optional<int> mesh = {}) {
    assets::GlbNode result;
    result.name = std::move(name);
    result.translation = translation;
    result.mesh = mesh;
    return result;
}

} // namespace

assets::GlbDescription makeCrtFixture() {
    assets::GlbDescription description;
    assets::GlbMaterial plastic;
    plastic.name = "Fixture_Plastic";
    plastic.baseColorFactor = glm::vec4(1.0f); // the texture holds the color
    plastic.metallicFactor = 0.0f;
    plastic.roughnessFactor = 0.55f;
    plastic.baseColorImage = plasticTexture();
    assets::GlbMaterial glass;
    glass.name = "Fixture_Glass";
    glass.baseColorFactor = glm::vec4(core::srgbToLinear(glm::vec3(42.0f, 48.0f, 46.0f) / 255.0f), 1.0f);
    glass.metallicFactor = 0.0f;
    glass.roughnessFactor = 0.3f;
    description.materials = {plastic, glass};

    description.meshes.push_back(assets::toGlbMesh(bodyBox("Body"), 0));
    assets::GlbMesh screen = assets::makeQuadMesh("Screen", kScreenWidth, kScreenHeight);
    screen.material = 1;
    description.meshes.push_back(std::move(screen));
    description.meshes.push_back(assets::toGlbMesh(bodyBox("Collider")));

    const float front = 0.5f * kCrtFixtureSize.z;
    assets::GlbNode root = node("Root");
    root.children = {1, 2, 3, 4, 5};
    description.nodes = {root,
                         node("Body", glm::vec3(0.0f), 0),
                         node("ScreenSurface", glm::vec3(0.0f, kScreenCenterY, front), 1),
                         node("Anchor_TV_Stand"),
                         node("Anchor_ScreenLook", glm::vec3(0.0f, kScreenCenterY, front + kLookDistance)),
                         node("ColliderProxy", glm::vec3(0.0f), 2)};
    return description;
}

} // namespace rv::preview
