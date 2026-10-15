#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/assets/model_data.hpp>
#include <rv/core/aabb.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::gl {

class MaterialRegistry;
class MeshRegistry;
class TextureCache;

/// A glTF model (assets::ModelData) on the GPU, ready to be placed any number of times:
///  - meshes in a MeshRegistry under "<key>#mesh<index>" (one per loaded primitive, shared by the nodes that
///    instance it and by every GpuModel created with the same key);
///  - materials in a MaterialRegistry under "<key>#material<index>", and "<key>#default" for primitives without one
///    (the glTF default material: white, metallic 1, roughness 1), named after the material, then the model file
///    ("Fixture_Plastic (crt_fixture.glb#material0)"); their images go through the TextureCache as "<key>#image<index>"
///    (base color and emissive sRGB, the others linear, repeating like glTF's default sampler);
///  - the draw items of the model's active scene with their model-space transforms (node world matrices), and its
///    sockets (style §9.2: anchors, screen surfaces, collider proxies) and bounds.
/// The registries and the cache own the GPU objects: a GpuModel holds handles into them and stays valid while they
/// live. Main thread only.
class GpuModel {
public:
    /// One primitive of the model at its place in the model.
    struct DrawItem {
        scene::MeshHandle mesh;
        scene::MaterialHandle material;
        scene::ShaderKind shader = scene::ShaderKind::Pbr;
        /// Model space (the model root) <- primitive space.
        glm::mat4 transform{1.0f};
    };

    GpuModel() = default;

    /// Registers the meshes, materials and images of `model`, identified by `key` (its path). Never fails: a primitive
    /// whose mesh cannot be uploaded is logged and left out. The materials are not uploaded: call
    /// MaterialRegistry::upload() before drawing.
    [[nodiscard]] static GpuModel create(std::string_view key, const assets::ModelData& model, MeshRegistry& meshes,
                                         MaterialRegistry& materials, TextureCache& textures);

    /// Appends one render item per draw item, placed by `transform` (model space -> world).
    void appendRenderItems(const glm::mat4& transform, bool castsShadow, std::vector<scene::RenderItem>& items) const;

    /// The MeshRegistry key of primitive `meshIndex` (into assets::ModelData::meshes) of the model registered with
    /// `key`: "<key>#mesh<meshIndex>".
    [[nodiscard]] static std::string meshKey(std::string_view key, std::uint32_t meshIndex);

    [[nodiscard]] const std::string& key() const noexcept { return m_key; }
    [[nodiscard]] std::span<const DrawItem> drawItems() const noexcept { return m_drawItems; }
    [[nodiscard]] std::span<const geometry::Socket> sockets() const noexcept { return m_sockets; }
    /// Model-space bounds of the drawn primitives.
    [[nodiscard]] const core::Aabb& bounds() const noexcept { return m_bounds; }

private:
    std::string m_key;
    std::vector<DrawItem> m_drawItems;
    std::vector<geometry::Socket> m_sockets;
    core::Aabb m_bounds;
};

} // namespace rv::gl
