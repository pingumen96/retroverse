#include <rv/render/material_set.hpp>

#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include <fmt/format.h>

#include <rv/render/material_uniforms.hpp>

namespace rv::gl {

Expected<MaterialSet> MaterialSet::create(const TextureDefaults& defaults, float anisotropy) {
    MaterialSet set;
    set.m_defaults = &defaults;
    for (std::size_t index = 0; index < kTextureWrapCount; ++index) {
        SamplingDesc desc;
        desc.wrap = static_cast<TextureWrap>(index);
        desc.filter = TextureFilter::Trilinear;
        desc.anisotropy = anisotropy;
        auto sampler = Sampler::create(desc, fmt::format("Sampler.Material.{}", toString(desc.wrap)));
        if (!sampler) {
            return propagate(sampler.error());
        }
        set.m_samplers[index] = std::move(*sampler);
    }
    return set;
}

scene::MaterialHandle MaterialSet::add(std::string name, const assets::MaterialDesc& desc,
                                       const MaterialTextures& textures) {
    GpuMaterial material;
    material.name = std::move(name);
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
    m_materials.push_back(std::move(material));
    return scene::MaterialHandle{static_cast<std::uint32_t>(m_materials.size() - 1)};
}

Status MaterialSet::upload(int uniformBufferOffsetAlignment, std::string_view debugName) {
    if (m_materials.empty()) {
        return makeError(ErrorCode::InvalidArgument, "no material to upload", std::string(debugName));
    }
    if (uniformBufferOffsetAlignment <= 0) {
        return makeError(
            ErrorCode::InvalidArgument,
            fmt::format("uniform buffer offset alignment {} is not positive", uniformBufferOffsetAlignment),
            std::string(debugName));
    }
    const auto alignment = static_cast<std::size_t>(uniformBufferOffsetAlignment);
    const std::size_t stride = (sizeof(MaterialUniforms) + alignment - 1) / alignment * alignment;
    std::vector<std::byte> bytes(stride * m_materials.size());
    for (std::size_t index = 0; index < m_materials.size(); ++index) {
        std::memcpy(bytes.data() + index * stride, &m_materials[index].uniforms, sizeof(MaterialUniforms));
    }
    auto buffer = Buffer::create(std::span<const std::byte>(bytes), 0, debugName);
    if (!buffer) {
        return propagate(buffer.error());
    }
    m_buffer = std::move(*buffer);
    m_stride = stride;
    m_uploadedCount = m_materials.size();
    return ok();
}

void MaterialSet::bind(scene::MaterialHandle handle) const {
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
