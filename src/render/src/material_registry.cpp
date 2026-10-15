#include <rv/render/material_registry.hpp>

#include <cstring>
#include <utility>

#include <fmt/format.h>

#include <rv/render/material_uniforms.hpp>
#include <rv/render/texture_cache.hpp>

namespace rv::gl {
namespace {

/// Texture of one material slot, or null when the slot has none.
const Texture2D* slotTexture(const assets::TextureRef& reference, ColorSpace colorSpace, TextureCache& textures,
                             std::string_view imageKeyPrefix, std::span<const assets::ImageData> images) {
    if (reference.empty()) {
        return nullptr;
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

std::optional<scene::MaterialHandle> MaterialRegistry::find(std::string_view key) const {
    if (const auto found = m_keys.find(key); found != m_keys.end()) {
        return scene::MaterialHandle{found->second};
    }
    return std::nullopt;
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

void MaterialRegistry::bind(scene::MaterialHandle handle) const {
    if (handle.value >= m_uploadedCount) {
        return;
    }
    const GpuMaterial& material = m_materials[handle.value];
    glBindBufferRange(GL_UNIFORM_BUFFER, toIndex(UniformBinding::Material), m_buffer.id(),
                      static_cast<GLintptr>(handle.value * m_stride),
                      static_cast<GLsizeiptr>(sizeof(MaterialUniforms)));
    glBindTextures(0, static_cast<GLsizei>(kMaterialTextureSlotCount), material.textures.data());
    glBindSamplers(0, static_cast<GLsizei>(kMaterialTextureSlotCount), material.samplers.data());
}

} // namespace rv::gl
