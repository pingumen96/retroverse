#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glad/gl.h>

#include <rv/assets/image_data.hpp>
#include <rv/assets/material_desc.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/material_uniforms.hpp>
#include <rv/render/sampler.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/texture_defaults.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::gl {

class TextureCache;

/// Textures of one material, indexed by MaterialTextureSlot; null where the material has no texture (the slot's
/// neutral default is bound instead and the shaders do not sample it).
using MaterialTextures = std::array<const Texture2D*, kMaterialTextureSlotCount>;

/// The textures `material` names, by slot (base color and emissive in sRGB, the data slots linear), from `textures`:
///  - TextureRef::procedural: a procedural surface texture (TextureCache::procedural), repeating;
///  - TextureRef::path: an asset file, repeating like the surfaces such textures tile;
///  - TextureRef::imageIndex: `images[index]`, a decoded image of a glTF model, cached under the key
///    "<imageKeyPrefix>#image<index>" and repeating like glTF's default sampler. An index without such an image is
///    logged and drawn as the checker.
/// Empty references leave their slot null.
[[nodiscard]] MaterialTextures resolveMaterialTextures(const assets::MaterialDesc& material, TextureCache& textures,
                                                       std::string_view imageKeyPrefix = {},
                                                       std::span<const assets::ImageData> images = {});

/// A material ready to draw.
struct GpuMaterial {
    /// The label it was added with, for logs and the developer overlay.
    std::string name;
    /// The key it was registered under (MaterialRegistry::add): the full identity behind the name, such as the path
    /// of the model a glTF material comes from.
    std::string key;
    /// The description the material was registered with. Live edits (MaterialRegistry::setFactors) change `uniforms`
    /// only, so this is also what reverting them goes back to.
    assets::MaterialDesc source;
    scene::ShaderKind shader = scene::ShaderKind::Pbr;
    assets::AlphaMode alphaMode = assets::AlphaMode::Opaque;
    bool doubleSided = false;
    MaterialUniforms uniforms;
    /// Texture bound to the unit of each slot: the material's texture, or TextureDefaults::neutral(slot).
    std::array<GLuint, kMaterialTextureSlotCount> textures{};
    /// The material's own texture of each slot (what the developer overlay shows), null where it has none and the
    /// slot's neutral default is bound. Owned by the TextureCache (or the TextureDefaults: the checker of a source that
    /// failed).
    MaterialTextures textureSources{};
    /// Sampler bound next to it: the registry's sampler for the texture's wrap mode.
    std::array<GLuint, kMaterialTextureSlotCount> samplers{};
};

/// The materials of the scene on the GPU, behind the scene::MaterialHandle of the render items: every
/// MaterialUniforms block in one uniform buffer of fixed size (each at an offset aligned to
/// GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT; live edits, setFactors, rewrite single blocks), and per material the textures
/// and samplers of its five slots. Samplers carry the renderer's filtering quality (trilinear, anisotropy from the
/// device capabilities) and the wrap mode of the texture they are bound with, so pictures stay clamped and surfaces
/// tile.
///
/// Materials are registered under keys that identify them (a library material's name, "<object>/<role>@WxH" for the
/// picture instance of a part, "<model>#material<i>" for a glTF material): equal keys share one material. Usage:
/// add() materials, upload(), then bind() before drawing with one; materials added after an upload are drawable after
/// the next one. The textures must outlive the registry (TextureCache, TextureDefaults). Main thread only.
class MaterialRegistry {
public:
    MaterialRegistry() = default;

    /// Creates the samplers; `anisotropy` is already clamped to the device limit (clampAnisotropy). `defaults` fill
    /// the slots without a texture and must outlive the registry.
    [[nodiscard]] static Expected<MaterialRegistry> create(const TextureDefaults& defaults, float anisotropy);

    /// The handle of `key`: registered before (unchanged), or a new material named `label` (logs, debugging) with the
    /// shader kind, alpha mode and sidedness of `desc`, and uniforms from makeMaterialUniforms(desc, texture bits of
    /// the non-null `textures`).
    [[nodiscard]] scene::MaterialHandle add(std::string_view key, std::string label, const assets::MaterialDesc& desc,
                                            const MaterialTextures& textures);
    /// The handle registered under `key`, if any.
    [[nodiscard]] std::optional<scene::MaterialHandle> find(std::string_view key) const;

    /// Forgets every material and releases the uniform buffer (the samplers stay): the handles returned so far are no
    /// longer valid, and render items that use them must not be drawn any more. For new content in the same renderer
    /// (the asset preview loads another model).
    void clear() noexcept;

    /// Writes every block into a new uniform buffer, when materials were added since the last upload (a no-op
    /// otherwise). Errors: InvalidArgument for an empty registry or a bad alignment, and the errors of Buffer::create.
    [[nodiscard]] Status upload(int uniformBufferOffsetAlignment, std::string_view debugName);

    /// Replaces the base color, metallic, roughness and emissive strength of `handle` (withFactors with its source's
    /// emissive factor; its shader, alpha mode and textures stay), for live editing in the developer overlay: the
    /// draws issued afterwards use them, nothing is saved. An uploaded material's block is rewritten at once, the
    /// others go up with the next upload(). Errors: InvalidArgument for an unknown handle, and the errors of
    /// Buffer::update.
    [[nodiscard]] Status setFactors(scene::MaterialHandle handle, const MaterialFactors& factors);

    /// Binds the uniform block of `handle` to UniformBinding::Material and its textures and samplers to the texture
    /// units 0..kMaterialTextureSlotCount-1. Does nothing for a handle outside the uploaded materials.
    void bind(scene::MaterialHandle handle) const;

    /// The material of a handle returned by add() (std::out_of_range otherwise).
    [[nodiscard]] const GpuMaterial& material(scene::MaterialHandle handle) const {
        return m_materials.at(handle.value);
    }
    /// Drawing pass of a handle (scene::drawPassOf its alpha mode); Opaque for an unknown handle.
    [[nodiscard]] scene::DrawPass pass(scene::MaterialHandle handle) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return m_materials.size(); }
    /// Materials that can be bound: those of the last upload.
    [[nodiscard]] std::size_t uploadedCount() const noexcept { return m_uploadedCount; }
    /// Bytes between two blocks in the uniform buffer.
    [[nodiscard]] std::size_t stride() const noexcept { return m_stride; }
    /// Size of the uniform buffer of the last upload, in bytes (textures are the TextureCache's).
    [[nodiscard]] std::size_t gpuBytes() const noexcept { return m_buffer.size(); }

private:
    const TextureDefaults* m_defaults = nullptr;
    std::array<Sampler, kTextureWrapCount> m_samplers;
    std::vector<GpuMaterial> m_materials;
    std::map<std::string, std::uint32_t, std::less<>> m_keys;
    Buffer m_buffer;
    std::size_t m_stride = 0;
    std::size_t m_uploadedCount = 0;
};

} // namespace rv::gl
