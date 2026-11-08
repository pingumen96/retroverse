#include "support/fixture_console.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include <rv/assets/glb_writer.hpp>
#include <rv/assets/image_data.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>

namespace rv::test {

namespace {

/// Diagonal stripes of two teals with a thin ivory line between them: the body's picture.
assets::ImageData stripes(int side) {
    assets::ImageData image;
    image.width = side;
    image.height = side;
    image.channels = 4;
    image.pixels.resize(static_cast<std::size_t>(side) * static_cast<std::size_t>(side) * 4);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            const int band = ((x + y) / std::max(1, side / 8)) % 2;
            const bool line = ((x + y) % std::max(2, side / 8)) < std::max(1, side / 64);
            const std::uint8_t r = line ? 236 : (band != 0 ? 40 : 28);
            const std::uint8_t g = line ? 230 : (band != 0 ? 150 : 118);
            const std::uint8_t b = line ? 214 : (band != 0 ? 140 : 112);
            const std::size_t at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(side) + static_cast<std::size_t>(x)) * 4;
            image.pixels[at] = r;
            image.pixels[at + 1] = g;
            image.pixels[at + 2] = b;
            image.pixels[at + 3] = 255;
        }
    }
    return image;
}

} // namespace

Status writeFixtureConsole(const std::filesystem::path& file, const FixtureConsoleOptions& options) {
    auto prefab = geometry::buildConsoleCartridge();
    if (!prefab) {
        return propagate(prefab.error());
    }
    assets::GlbDescription glb;
    // Material 0: the body, striped; 1: every other part, a dark graphite.
    assets::GlbMaterial body;
    body.name = "fixture_body";
    body.roughnessFactor = 0.55F;
    body.baseColorImage = stripes(options.imageSide);
    glb.materials.push_back(std::move(body));
    assets::GlbMaterial trim;
    trim.name = "fixture_trim";
    trim.baseColorFactor = glm::vec4(0.08F, 0.09F, 0.10F, 1.0F);
    trim.roughnessFactor = 0.4F;
    glb.materials.push_back(std::move(trim));

    assets::GlbNode root;
    root.name = "FixtureConsole";
    root.scale = glm::vec3(options.scale);
    glb.nodes.push_back(root);
    for (std::size_t index = 0; index < prefab->parts.size(); ++index) {
        const geometry::PrefabPart& part = prefab->parts[index];
        const bool isBody = part.materialRole.find("body") != std::string::npos ||
                            part.materialRole.find("shell") != std::string::npos;
        glb.meshes.push_back(assets::toGlbMesh(part.mesh, isBody ? 0 : 1));
        assets::GlbNode node;
        node.name = "part_" + std::to_string(index);
        node.mesh = static_cast<int>(glb.meshes.size() - 1);
        glb.nodes.push_back(std::move(node));
        glb.nodes[0].children.push_back(static_cast<int>(glb.nodes.size() - 1));
    }
    if (options.slotAnchor) {
        if (const geometry::Socket* slot = geometry::findSocket(prefab->sockets, "CartridgeSlot")) {
            assets::GlbNode anchor;
            anchor.name = "Anchor_CartridgeSlot";
            anchor.matrix = slot->transform;
            glb.nodes.push_back(std::move(anchor));
            glb.nodes[0].children.push_back(static_cast<int>(glb.nodes.size() - 1));
        }
    }
    glb.sceneRoots = {0};
    return assets::writeGlb(file, glb);
}

} // namespace rv::test
