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
class Texture2DArray;

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

/// The texture array whose layers multiply a layered material's base color (material_flags::kLayeredBaseColor): an
/// array of the collection's fronts or spines (CollectionArrays::array), sampled at the layer the instance record names.
struct LayeredBaseColor {
    /// Must outlive the material; its storage may be replaced (Texture2DArray::grow): bind() reads its current id.
    const Texture2DArray* array = nullptr;
    /// The record's spine layer (material_flags::kSpineLayer), else its front layer.
    bool spineLayer = false;
};

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
    /// The array a layered material binds on CollectionTextureUnit::LayeredBaseColor (null for every other material),
    /// with the registry's clamped sampler.
    const Texture2DArray* layeredBaseColor = nullptr;
    /// Textures bound instead of `textures` (MaterialRegistry::setTextureOverride), 0 where the material's own is.
    std::array<GLuint, kMaterialTextureSlotCount> textureOverrides{};
};

/// The materials of the scene on the GPU, behind the scene::MaterialHandle of the render items: every
/// MaterialUniforms block in one uniform buffer of fixed size (each at an offset aligned to
/// GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT; live edits, setFactors, rewrite single blocks), and per material the textures
/// and samplers of its five slots. Samplers carry the renderer's filtering quality (trilinear, anisotropy from the
/// device capabilities) and the wrap mode of the texture they are bound with, so pictures stay clamped and surfaces
/// tile.
///
/// Materials are registered under keys that identify them (a library material's name, "<object>/<role>@WxH#<picture>"
/// for the picture instance of a part, "<model>#material<i>" for a glTF material): equal keys share one material. Usage:
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
    /// add() for a layered material (ADR-0011 §4): its base color also multiplies the layer of `layered.array` that
    /// each instance's record names (material_flags::kLayeredBaseColor, plus kSpineLayer for spines), so that one
    /// material, and one batch, draws every item of the array with its own picture. A null array registers an ordinary
    /// material (and logs it). Registered before under `key`: that handle, unchanged.
    [[nodiscard]] scene::MaterialHandle addLayered(std::string_view key, std::string label,
                                                   const assets::MaterialDesc& desc, const MaterialTextures& textures,
                                                   const LayeredBaseColor& layered);
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

    /// Draws `slot` of `handle` with the texture object `texture` instead of the material's own from the next bind() on
    /// (I3 step 11: the hero CRT's screen shows the video's texture or a still picture in place of its colour bars,
    /// ADR-0012 §7), with the slot's sampler, the uniforms unchanged; 0 goes back to the material's own texture. The
    /// caller keeps `texture` alive while it is set, and sets it again when the object changes (gl::VideoTexture
    /// reallocates on a size change). The slot must have a texture of the material's own (its texture bit is set, so
    /// the shaders sample it) and the texture its colour space. Errors: InvalidArgument for an unknown handle or a slot
    /// without a texture of its own.
    [[nodiscard]] Status setTextureOverride(scene::MaterialHandle handle, MaterialTextureSlot slot, GLuint texture);
    /// The override of `slot` of `handle` (0 for none, or an unknown handle).
    [[nodiscard]] GLuint textureOverride(scene::MaterialHandle handle, MaterialTextureSlot slot) const noexcept;

    /// The part of the screen `handle` draws its picture in (I5 step 4, ADR-0016 §7: a clip letterboxed or pillarboxed,
    /// MaterialUniforms::pictureRect with material_flags::kPictureRect): `rect` is its corner and size in the screen's
    /// texture coordinates; none, or the whole screen (0, 0, 1, 1), clears the flag, so that the material's block is
    /// again exactly what it was registered with. The block is rewritten only when it changes (an uploaded one at once,
    /// the others with the next upload()). Errors: InvalidArgument for an unknown handle or a rectangle that is not
    /// finite or has no area, and the errors of Buffer::update.
    [[nodiscard]] Status setPictureRect(scene::MaterialHandle handle, std::optional<glm::vec4> rect);

    /// Binds the uniform block of `handle` to UniformBinding::Material and its textures (or their overrides) and
    /// samplers to the texture units 0..kMaterialTextureSlotCount-1, and a layered material's array to
    /// CollectionTextureUnit::LayeredBaseColor. Does nothing for a handle outside the uploaded materials.
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
