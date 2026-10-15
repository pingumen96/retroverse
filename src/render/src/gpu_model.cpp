#include <rv/render/gpu_model.hpp>

#include <optional>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/render/material_registry.hpp>
#include <rv/render/mesh_registry.hpp>
#include <rv/render/texture_cache.hpp>

namespace rv::gl {
namespace {

/// The glTF default material, for primitives without one: white, fully metallic and rough (glTF 2.0 §3.9.2).
assets::MaterialDesc defaultGltfMaterial() {
    assets::MaterialDesc material;
    material.name = "glTF default material";
    material.baseColorFactor = glm::vec4(1.0f);
    material.metallicFactor = 1.0f;
    material.roughnessFactor = 1.0f;
    return material;
}

/// What follows the last path separator of a registry key: "crt.glb#material0" of "models/crt.glb#material0".
std::string_view fileNameOf(std::string_view key) noexcept {
    const std::size_t separator = key.find_last_of("/\\");
    return separator == std::string_view::npos ? key : key.substr(separator + 1);
}

} // namespace

std::string GpuModel::meshKey(std::string_view key, std::uint32_t meshIndex) {
    return fmt::format("{}#mesh{}", key, meshIndex);
}

GpuModel GpuModel::create(std::string_view key, const assets::ModelData& model, MeshRegistry& meshes,
                          MaterialRegistry& materials, TextureCache& textures) {
    GpuModel gpu;
    gpu.m_key = std::string(key);
    gpu.m_sockets = model.sockets;
    gpu.m_bounds = model.bounds;

    // Meshes and materials are registered at first use, so unused ones never reach the GPU.
    std::vector<std::optional<scene::MeshHandle>> meshHandles(model.meshes.size());
    std::vector<bool> meshTried(model.meshes.size(), false);
    std::vector<std::optional<scene::MaterialHandle>> materialHandles(model.materials.size());
    std::optional<scene::MaterialHandle> defaultMaterial;
    // Named after the glTF material first, so that the materials of one model read apart in lists that cut long names.
    const auto registerMaterial = [&](const std::string& materialKey, const assets::MaterialDesc& desc) {
        return materials.add(materialKey, fmt::format("{} ({})", desc.name, fileNameOf(materialKey)), desc,
                             resolveMaterialTextures(desc, textures, key, model.images));
    };

    for (const assets::ModelDrawItem& item : model.drawItems) {
        if (item.meshIndex >= model.meshes.size()) {
            continue;
        }
        if (!meshTried[item.meshIndex]) {
            meshTried[item.meshIndex] = true;
            const geometry::MeshData& mesh = model.meshes[item.meshIndex];
            auto handle = meshes.add(meshKey(key, item.meshIndex), mesh);
            if (handle) {
                meshHandles[item.meshIndex] = *handle;
            } else {
                spdlog::error("Model '{}': primitive '{}' cannot be uploaded and is not drawn: {}", key, mesh.name,
                              handle.error().toString());
            }
        }
        if (!meshHandles[item.meshIndex]) {
            continue;
        }

        scene::MaterialHandle material;
        if (item.material && *item.material < model.materials.size()) {
            std::optional<scene::MaterialHandle>& known = materialHandles[*item.material];
            if (!known) {
                known = registerMaterial(fmt::format("{}#material{}", key, *item.material),
                                         model.materials[*item.material]);
            }
            material = *known;
        } else {
            if (!defaultMaterial) {
                defaultMaterial = registerMaterial(fmt::format("{}#default", key), defaultGltfMaterial());
            }
            material = *defaultMaterial;
        }
        gpu.m_drawItems.push_back(
            DrawItem{*meshHandles[item.meshIndex], material, materials.material(material).shader, item.world});
    }
    return gpu;
}

void GpuModel::appendRenderItems(const glm::mat4& transform, bool castsShadow,
                                 std::vector<scene::RenderItem>& items) const {
    for (const DrawItem& item : m_drawItems) {
        items.push_back(
            scene::RenderItem{item.mesh, item.material, item.shader, transform * item.transform, castsShadow});
    }
}

} // namespace rv::gl
