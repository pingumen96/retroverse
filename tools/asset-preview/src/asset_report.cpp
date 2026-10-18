#include <rv/preview/asset_report.hpp>

#include <algorithm>
#include <array>
#include <set>
#include <string_view>
#include <utility>

#include <rv/geometry/socket.hpp>
#include <rv/render/texture_memory.hpp>

namespace rv::preview {
namespace {

/// A material slot, its name and whether its texture is sampled as sRGB.
struct Slot {
    assets::TextureRef assets::MaterialDesc::* texture;
    std::string_view name;
    bool srgb;
};

constexpr std::array<Slot, 5> kSlots{{
    {&assets::MaterialDesc::baseColorTexture, "base color", true},
    {&assets::MaterialDesc::metallicRoughnessTexture, "metallic-roughness", false},
    {&assets::MaterialDesc::normalTexture, "normal", false},
    {&assets::MaterialDesc::occlusionTexture, "occlusion", false},
    {&assets::MaterialDesc::emissiveTexture, "emissive", true},
}};

void reportModel(const assets::ModelData& model, AssetReport& report) {
    report.nodes = model.stats.nodes;
    report.meshes = model.stats.meshes;
    report.materials = model.stats.materials;
    report.uniqueTriangles = model.stats.triangles;
    report.extensions = model.stats.extensionsUsed;
    report.primitives = model.drawItems.size();
    for (const assets::ModelDrawItem& item : model.drawItems) {
        if (item.meshIndex < model.meshes.size()) {
            report.triangles += model.meshes[item.meshIndex].triangleCount();
            report.vertices += model.meshes[item.meshIndex].vertices.size();
        }
    }
    for (std::uint32_t index = 0; index < model.images.size(); ++index) {
        const assets::ImageData& image = model.images[index];
        TextureReport texture;
        texture.image = index;
        texture.size = glm::ivec2(image.width, image.height);
        texture.channels = image.channels;
        std::set<bool> colorSpaces; // sRGB or linear: each is an upload of its own
        for (const assets::MaterialDesc& material : model.materials) {
            for (const Slot& slot : kSlots) {
                if ((material.*slot.texture).imageIndex == index) {
                    texture.usage += texture.usage.empty() ? "" : ", ";
                    texture.usage += std::string(slot.name) + " of " + material.name;
                    colorSpaces.insert(slot.srgb);
                }
            }
        }
        const std::size_t chain = gl::mipChainBytes(texture.size, gl::fullMipLevelCount(texture.size));
        texture.gpuBytes = chain * colorSpaces.size();
        report.textureBytes += texture.gpuBytes;
        report.textures.push_back(std::move(texture));
    }
}

void reportPrefab(const geometry::ProceduralPrefab& prefab, AssetReport& report) {
    report.nodes = 1;
    report.meshes = prefab.parts.size();
    report.primitives = prefab.parts.size();
    std::set<std::string> roles;
    for (const geometry::PrefabPart& part : prefab.parts) {
        report.triangles += part.mesh.triangleCount();
        report.vertices += part.mesh.vertices.size();
        roles.insert(part.materialRole);
    }
    report.uniqueTriangles = report.triangles;
    report.materials = roles.size();
}

} // namespace

AssetReport buildAssetReport(const PreviewSubject& subject) {
    AssetReport report;
    if (subject.model) {
        reportModel(*subject.model, report);
    } else if (subject.prefab) {
        reportPrefab(*subject.prefab, report);
    }
    report.sockets = subject.sockets.size();
    for (const geometry::SlotLevel& level : geometry::slotLevels(subject.sockets)) {
        const auto same = std::find_if(report.slots.begin(), report.slots.end(), [&](const SlotReport& slots) {
            return glm::all(glm::lessThan(glm::abs(slots.size - level.slotSize), glm::vec3(1e-5f)));
        });
        SlotReport& slots = same != report.slots.end() ? *same : report.slots.emplace_back(SlotReport{level.slotSize});
        slots.slots += static_cast<std::size_t>(level.count);
        ++slots.levels;
    }
    if (subject.target && !subject.placeholder) {
        report.size = subject.bounds.size();
    }
    return report;
}

} // namespace rv::preview
