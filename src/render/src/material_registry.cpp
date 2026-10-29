#include <rv/render/material_registry.hpp>

#include <cmath>
#include <cstring>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/render/material_uniforms.hpp>
#include <rv/render/texture_array.hpp>
#include <rv/render/texture_cache.hpp>

namespace rv::gl {
namespace {

/// Texture of one material slot, or null when the slot has none.
const Texture2D* slotTexture(const assets::TextureRef& reference, ColorSpace colorSpace, TextureCache& textures,
                             std::string_view imageKeyPrefix, std::span<const assets::ImageData> images) {
    if (reference.empty()) {
        return nullptr;
    }
    if (!reference.procedural.empty()) {
        return &textures.procedural(reference.procedural, colorSpace);
    }
    if (!reference.path.empty()) {
        return &textures.file(reference.path, colorSpace, TextureWrap::Repeat);
    }
    const std::uint32_t index = *reference.imageIndex;
    const std::string key = fmt::format("{}#image{}", imageKeyPrefix, index);
    if (index >= images.size()) {
        // An empty image: the cache logs the key once as unavailable and serves the checker (style §21).
        return &textures.image(key, assets::ImageData{}, colorSpace, TextureWrap::Repeat);
    }
    return &textures.image(key, images[index], colorSpace, TextureWrap::Repeat);
}

} // namespace

MaterialTextures resolveMaterialTextures(const assets::MaterialDesc& material, TextureCache& textures,
                                         std::string_view imageKeyPrefix, std::span<const assets::ImageData> images) {
    MaterialTextures slots{};
    const auto set = [&](MaterialTextureSlot slot, const assets::TextureRef& reference, ColorSpace space) {
        slots[toIndex(slot)] = slotTexture(reference, space, textures, imageKeyPrefix, images);
    };
    set(MaterialTextureSlot::BaseColor, material.baseColorTexture, ColorSpace::Srgb);
    set(MaterialTextureSlot::MetallicRoughness, material.metallicRoughnessTexture, ColorSpace::Linear);
    set(MaterialTextureSlot::Normal, material.normalTexture, ColorSpace::Linear);
    set(MaterialTextureSlot::Occlusion, material.occlusionTexture, ColorSpace::Linear);
    set(MaterialTextureSlot::Emissive, material.emissiveTexture, ColorSpace::Srgb);
    return slots;
}

Expected<MaterialRegistry> MaterialRegistry::create(const TextureDefaults& defaults, float anisotropy) {
    MaterialRegistry registry;
    registry.m_defaults = &defaults;
    for (std::size_t index = 0; index < kTextureWrapCount; ++index) {
        SamplingDesc desc;
        desc.wrap = static_cast<TextureWrap>(index);
        desc.filter = TextureFilter::Trilinear;
        desc.anisotropy = anisotropy;
        auto sampler = Sampler::create(desc, fmt::format("Sampler.Material.{}", toString(desc.wrap)));
        if (!sampler) {
            return propagate(sampler.error());
        }
        registry.m_samplers[index] = std::move(*sampler);
    }
    return registry;
}

scene::MaterialHandle MaterialRegistry::add(std::string_view key, std::string label, const assets::MaterialDesc& desc,
                                            const MaterialTextures& textures) {
    if (const std::optional<scene::MaterialHandle> known = find(key)) {
        return *known;
    }
    GpuMaterial material;
    material.name = std::move(label);
    material.key = std::string(key);
    material.source = desc;
    material.shader = shaderKindOf(desc);
    material.alphaMode = desc.alphaMode;
    material.doubleSided = desc.doubleSided;
    std::uint32_t textureFlags = 0;
    for (std::uint32_t slot = 0; slot < kMaterialTextureSlotCount; ++slot) {
        const auto materialSlot = static_cast<MaterialTextureSlot>(slot);
        const Texture2D* texture = textures[slot];
        if (texture != nullptr && texture->valid()) {
            textureFlags |= material_flags::textureBit(materialSlot);
            material.textureSources[slot] = texture;
        } else {
            texture = m_defaults != nullptr ? &m_defaults->neutral(materialSlot) : nullptr;
        }
        material.textures[slot] = texture != nullptr ? texture->id() : 0;
        const auto wrap = texture != nullptr ? texture->desc().sampling.wrap : TextureWrap::Repeat;
        material.samplers[slot] = m_samplers[static_cast<std::size_t>(wrap)].id();
    }
    material.uniforms = makeMaterialUniforms(desc, textureFlags);
    const auto index = static_cast<std::uint32_t>(m_materials.size());
    m_materials.push_back(std::move(material));
    m_keys.emplace(std::string(key), index);
    return scene::MaterialHandle{index};
}

scene::MaterialHandle MaterialRegistry::addLayered(std::string_view key, std::string label,
                                                   const assets::MaterialDesc& desc, const MaterialTextures& textures,
                                                   const LayeredBaseColor& layered) {
    if (const std::optional<scene::MaterialHandle> known = find(key)) {
        return *known;
    }
    if (layered.array == nullptr) {
        spdlog::error("Layered material '{}' has no texture array: drawn without its layers", label);
        return add(key, std::move(label), desc, textures);
    }
    // A spine is alpha-masked with a cutoff of 0 (I6 step 4): never cut by its alpha, but drawn by the masked depth and
    // outline programs, which bind it, so that every pass leaves out a face-out item's spine label
    // (scene::instance_flags::kNoSpineLabel, common/material.glsl's materialInstanceTest).
    assets::MaterialDesc layeredDesc = desc;
    if (layered.spineLayer && layeredDesc.alphaMode == assets::AlphaMode::Opaque) {
        layeredDesc.alphaMode = assets::AlphaMode::Mask;
        layeredDesc.alphaCutoff = 0.0f;
    }
    const scene::MaterialHandle handle = add(key, std::move(label), layeredDesc, textures);
    GpuMaterial& material = m_materials[handle.value];
    material.layeredBaseColor = layered.array;
    material.uniforms.flags |= material_flags::kLayeredBaseColor;
    if (layered.spineLayer) {
        material.uniforms.flags |= material_flags::kSpineLayer;
    }
    return handle;
}

std::optional<scene::MaterialHandle> MaterialRegistry::find(std::string_view key) const {
    if (const auto found = m_keys.find(key); found != m_keys.end()) {
        return scene::MaterialHandle{found->second};
    }
    return std::nullopt;
}

void MaterialRegistry::clear() noexcept {
    m_materials.clear();
    m_keys.clear();
    m_buffer = Buffer();
    m_stride = 0;
    m_uploadedCount = 0;
}

scene::DrawPass MaterialRegistry::pass(scene::MaterialHandle handle) const noexcept {
    return handle.value < m_materials.size() ? scene::drawPassOf(m_materials[handle.value].alphaMode)
                                             : scene::DrawPass::Opaque;
}

Status MaterialRegistry::upload(int uniformBufferOffsetAlignment, std::string_view debugName) {
    if (m_materials.empty()) {
        return makeError(ErrorCode::InvalidArgument, "no material to upload", std::string(debugName));
    }
    if (uniformBufferOffsetAlignment <= 0) {
        return makeError(
            ErrorCode::InvalidArgument,
            fmt::format("uniform buffer offset alignment {} is not positive", uniformBufferOffsetAlignment),
            std::string(debugName));
    }
    if (m_uploadedCount == m_materials.size()) {
        return ok();
    }
    const auto alignment = static_cast<std::size_t>(uniformBufferOffsetAlignment);
    const std::size_t stride = (sizeof(MaterialUniforms) + alignment - 1) / alignment * alignment;
    std::vector<std::byte> bytes(stride * m_materials.size());
    for (std::size_t index = 0; index < m_materials.size(); ++index) {
        std::memcpy(bytes.data() + index * stride, &m_materials[index].uniforms, sizeof(MaterialUniforms));
    }
    // Dynamic storage: setFactors() rewrites single blocks (developer overlay).
    auto buffer = Buffer::create(std::span<const std::byte>(bytes), GL_DYNAMIC_STORAGE_BIT, debugName);
    if (!buffer) {
        return propagate(buffer.error());
    }
    m_buffer = std::move(*buffer);
    m_stride = stride;
    m_uploadedCount = m_materials.size();
    return ok();
}

Status MaterialRegistry::setFactors(scene::MaterialHandle handle, const MaterialFactors& factors) {
    if (handle.value >= m_materials.size()) {
        return makeError(ErrorCode::InvalidArgument, "unknown material", fmt::format("material #{}", handle.value));
    }
    GpuMaterial& material = m_materials[handle.value];
    material.uniforms = withFactors(material.uniforms, factors, material.source.emissiveFactor);
    if (handle.value >= m_uploadedCount) {
        return ok();
    }
    return m_buffer.update(handle.value * m_stride,
                           std::as_bytes(std::span<const MaterialUniforms, 1>(&material.uniforms, 1)));
}

Status MaterialRegistry::setTextureOverride(scene::MaterialHandle handle, MaterialTextureSlot slot, GLuint texture) {
    if (handle.value >= m_materials.size()) {
        return makeError(ErrorCode::InvalidArgument, "unknown material", fmt::format("material #{}", handle.value));
    }
    GpuMaterial& material = m_materials[handle.value];
    const std::size_t index = toIndex(slot);
    if (material.textureSources[index] == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the slot has no texture of its own to override",
                         fmt::format("material '{}', slot {}", material.name, index));
    }
    material.textureOverrides[index] = texture;
    return ok();
}

Status MaterialRegistry::setPictureRect(scene::MaterialHandle handle, std::optional<glm::vec4> rect) {
    if (handle.value >= m_materials.size()) {
        return makeError(ErrorCode::InvalidArgument, "unknown material", fmt::format("material #{}", handle.value));
    }
    if (rect && (!std::isfinite(rect->x) || !std::isfinite(rect->y) || !std::isfinite(rect->z) ||
                 !std::isfinite(rect->w) || rect->z <= 0.0f || rect->w <= 0.0f)) {
        return makeError(ErrorCode::InvalidArgument, "a picture rectangle without area",
                         fmt::format("material '{}'", m_materials[handle.value].name));
    }
    constexpr glm::vec4 kWhole{0.0f, 0.0f, 1.0f, 1.0f};
    const bool whole = !rect || *rect == kWhole;
    GpuMaterial& material = m_materials[handle.value];
    MaterialUniforms uniforms = material.uniforms;
    uniforms.pictureRect = whole ? kWhole : *rect;
    uniforms.flags = whole ? (uniforms.flags & ~material_flags::kPictureRect)
                           : (uniforms.flags | material_flags::kPictureRect);
    if (uniforms.pictureRect == material.uniforms.pictureRect && uniforms.flags == material.uniforms.flags) {
        return ok();
    }
    material.uniforms = uniforms;
    if (handle.value >= m_uploadedCount) {
        return ok();
    }
    return m_buffer.update(handle.value * m_stride,
                           std::as_bytes(std::span<const MaterialUniforms, 1>(&material.uniforms, 1)));
}

GLuint MaterialRegistry::textureOverride(scene::MaterialHandle handle, MaterialTextureSlot slot) const noexcept {
    return handle.value < m_materials.size() ? m_materials[handle.value].textureOverrides[toIndex(slot)] : 0;
}

void MaterialRegistry::bind(scene::MaterialHandle handle) const {
    if (handle.value >= m_uploadedCount) {
        return;
    }
    const GpuMaterial& material = m_materials[handle.value];
    glBindBufferRange(GL_UNIFORM_BUFFER, toIndex(UniformBinding::Material), m_buffer.id(),
                      static_cast<GLintptr>(handle.value * m_stride),
                      static_cast<GLsizeiptr>(sizeof(MaterialUniforms)));
    std::array<GLuint, kMaterialTextureSlotCount> textures = material.textures;
    for (std::size_t slot = 0; slot < kMaterialTextureSlotCount; ++slot) {
        if (material.textureOverrides[slot] != 0) {
            textures[slot] = material.textureOverrides[slot];
        }
    }
    glBindTextures(0, static_cast<GLsizei>(kMaterialTextureSlotCount), textures.data());
    glBindSamplers(0, static_cast<GLsizei>(kMaterialTextureSlotCount), material.samplers.data());
    if (material.layeredBaseColor != nullptr) {
        // Read at every bind: a grown array has new storage under the same object.
        const GLuint unit = toIndex(CollectionTextureUnit::LayeredBaseColor);
        const GLuint texture = material.layeredBaseColor->id();
        const GLuint sampler = m_samplers[static_cast<std::size_t>(TextureWrap::ClampToEdge)].id();
        glBindTextures(unit, 1, &texture);
        glBindSamplers(unit, 1, &sampler);
    }
}

} // namespace rv::gl
