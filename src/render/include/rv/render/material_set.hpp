#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <glad/gl.h>

#include <rv/assets/material_desc.hpp>
#include <rv/core/expected.hpp>
#include <rv/render/buffer.hpp>
#include <rv/render/sampler.hpp>
#include <rv/render/texture.hpp>
#include <rv/render/texture_defaults.hpp>
#include <rv/render/uniform_blocks.hpp>
#include <rv/scene/render_item.hpp>

namespace rv::gl {

/// Textures of one material, indexed by MaterialTextureSlot; null where the material has no texture (the slot's
/// neutral default is bound instead and the shaders do not sample it).
using MaterialTextures = std::array<const Texture2D*, kMaterialTextureSlotCount>;

/// A material ready to draw.
struct GpuMaterial {
    std::string name;
    scene::ShaderKind shader = scene::ShaderKind::Pbr;
    assets::AlphaMode alphaMode = assets::AlphaMode::Opaque;
    bool doubleSided = false;
    MaterialUniforms uniforms;
    /// Texture bound to the unit of each slot: the material's texture, or TextureDefaults::neutral(slot).
    std::array<GLuint, kMaterialTextureSlotCount> textures{};
    /// Sampler bound next to it: the set's sampler for the texture's wrap mode.
    std::array<GLuint, kMaterialTextureSlotCount> samplers{};
};

/// The materials of a scene on the GPU: every MaterialUniforms block in one immutable uniform buffer (each at an
/// offset aligned to GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT), and per material the textures and samplers of its five
/// slots. Samplers carry the renderer's filtering quality (trilinear, anisotropy from the device capabilities) and
/// the wrap mode of the texture they are bound with, so pictures stay clamped and surfaces tile.
///
/// Usage: add() every material, upload() once, then bind() before drawing with it. The textures must outlive the
/// set (TextureCache, TextureDefaults). Main thread only.
class MaterialSet {
public:
    MaterialSet() = default;

    /// Creates the samplers; `anisotropy` is already clamped to the device limit (clampAnisotropy). `defaults` fill
    /// the slots without a texture and must outlive the set.
    [[nodiscard]] static Expected<MaterialSet> create(const TextureDefaults& defaults, float anisotropy);

    /// Adds a material named `name` (logs, debugging): shader kind, alpha mode and sidedness of `desc`, uniforms from
    /// makeMaterialUniforms(desc, texture bits of the non-null `textures`). Returns its handle.
    [[nodiscard]] scene::MaterialHandle add(std::string name, const assets::MaterialDesc& desc,
                                            const MaterialTextures& textures);

    /// Writes every block into a new immutable uniform buffer; materials added later are not drawable until the next
    /// upload. Errors: InvalidArgument for an empty set or a bad alignment, and the errors of Buffer::create.
    [[nodiscard]] Status upload(int uniformBufferOffsetAlignment, std::string_view debugName);

    /// Binds the uniform block of `handle` to UniformBinding::Material and its textures and samplers to the texture
    /// units 0..kMaterialTextureSlotCount-1. Does nothing for a handle outside the uploaded materials.
    void bind(scene::MaterialHandle handle) const;

    [[nodiscard]] const GpuMaterial& material(scene::MaterialHandle handle) const {
        return m_materials.at(handle.value);
    }
    [[nodiscard]] std::size_t size() const noexcept { return m_materials.size(); }
    /// Bytes between two blocks in the uniform buffer.
    [[nodiscard]] std::size_t stride() const noexcept { return m_stride; }

private:
    const TextureDefaults* m_defaults = nullptr;
    std::array<Sampler, kTextureWrapCount> m_samplers;
    std::vector<GpuMaterial> m_materials;
    Buffer m_buffer;
    std::size_t m_stride = 0;
    std::size_t m_uploadedCount = 0;
};

} // namespace rv::gl
