#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include <rv/assets/material_desc.hpp>
#include <rv/render/material_uniforms.hpp>

using Catch::Approx;
using rv::assets::MaterialDesc;
namespace flags = rv::gl::material_flags;

TEST_CASE("render/material_uniforms: factors, emissive strength and flags", "[render][materials]") {
    MaterialDesc lamp;
    lamp.name = "MAT_Lamp_Shade";
    lamp.baseColorFactor = glm::vec4(0.8f, 0.7f, 0.5f, 1.0f);
    lamp.metallicFactor = 0.0f;
    lamp.roughnessFactor = 0.85f;
    lamp.emissiveFactor = glm::vec3(1.0f, 0.58f, 0.25f);
    lamp.emissiveStrength = 2.0f;
    lamp.normalScale = 0.5f;
    lamp.occlusionStrength = 0.7f;
    lamp.doubleSided = true;

    const rv::gl::MaterialUniforms block = rv::gl::makeMaterialUniforms(lamp, flags::kBaseColorTexture);
    CHECK(block.baseColorFactor == lamp.baseColorFactor);
    CHECK(block.emissive == glm::vec3(2.0f, 1.16f, 0.5f)); // emissive * strength
    CHECK(block.emissiveStrength == 2.0f);
    CHECK(block.roughness == 0.85f);
    CHECK(block.metallic == 0.0f);
    CHECK(block.normalScale == 0.5f);
    CHECK(block.occlusionStrength == 0.7f);
    CHECK(block.flags == (flags::kBaseColorTexture | flags::kDoubleSided));

    CHECK(rv::gl::shaderKindOf(lamp) == rv::scene::ShaderKind::Pbr);
    MaterialDesc screen;
    screen.shading = rv::assets::ShadingModel::Unlit;
    CHECK(rv::gl::shaderKindOf(screen) == rv::scene::ShaderKind::Unlit);
}

TEST_CASE("render/material_uniforms: alpha modes and texture bits", "[render][materials]") {
    MaterialDesc material;
    CHECK(rv::gl::makeMaterialUniforms(material, 0).flags == 0u);
    material.alphaMode = rv::assets::AlphaMode::Mask;
    material.alphaCutoff = 0.3f;
    rv::gl::MaterialUniforms block = rv::gl::makeMaterialUniforms(material, 0);
    CHECK(block.flags == flags::kAlphaMask);
    CHECK(block.alphaCutoff == 0.3f);
    material.alphaMode = rv::assets::AlphaMode::Blend;
    CHECK(rv::gl::makeMaterialUniforms(material, 0).flags == flags::kAlphaBlend);

    // Only texture bits pass through; each slot has its own bit.
    block = rv::gl::makeMaterialUniforms(MaterialDesc{}, 0xFFFFFFFFu);
    CHECK(block.flags == (flags::kBaseColorTexture | flags::kMetallicRoughnessTexture | flags::kNormalTexture |
                          flags::kOcclusionTexture | flags::kEmissiveTexture));
    CHECK(flags::textureBit(rv::gl::MaterialTextureSlot::BaseColor) == flags::kBaseColorTexture);
    CHECK(flags::textureBit(rv::gl::MaterialTextureSlot::MetallicRoughness) == flags::kMetallicRoughnessTexture);
    CHECK(flags::textureBit(rv::gl::MaterialTextureSlot::Normal) == flags::kNormalTexture);
    CHECK(flags::textureBit(rv::gl::MaterialTextureSlot::Occlusion) == flags::kOcclusionTexture);
    CHECK(flags::textureBit(rv::gl::MaterialTextureSlot::Emissive) == flags::kEmissiveTexture);
}

TEST_CASE("render/material_uniforms: values the shaders cannot use never reach them", "[render][materials]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    MaterialDesc broken;
    broken.baseColorFactor = glm::vec4(nan, -1.0f, 0.5f, 2.0f);
    broken.metallicFactor = 1.5f;
    broken.roughnessFactor = nan;
    broken.normalScale = inf;
    broken.occlusionStrength = -0.5f;
    broken.emissiveFactor = glm::vec3(1.0f, nan, -2.0f);
    broken.emissiveStrength = nan;
    broken.alphaCutoff = 7.0f;

    const rv::gl::MaterialUniforms block = rv::gl::makeMaterialUniforms(broken, 0);
    const MaterialDesc defaults;
    CHECK(block.baseColorFactor == glm::vec4(1.0f, 0.0f, 0.5f, 1.0f));
    CHECK(block.metallic == 1.0f);
    CHECK(block.roughness == defaults.roughnessFactor);
    CHECK(block.normalScale == defaults.normalScale);
    CHECK(block.occlusionStrength == 0.0f);
    CHECK(block.emissiveStrength == defaults.emissiveStrength);
    CHECK(block.emissive == glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(block.alphaCutoff == 1.0f);
}
