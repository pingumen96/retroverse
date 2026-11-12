#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
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

TEST_CASE("render/material_uniforms: live factor edits match a material with those factors", "[render][materials]") {
    MaterialDesc lamp;
    lamp.baseColorFactor = glm::vec4(0.8f, 0.7f, 0.5f, 1.0f);
    lamp.roughnessFactor = 0.85f;
    lamp.emissiveFactor = glm::vec3(1.0f, 0.58f, 0.25f);
    lamp.emissiveStrength = 2.0f;
    lamp.normalScale = 0.5f;
    lamp.alphaMode = rv::assets::AlphaMode::Mask;
    const std::uint32_t textures = flags::kBaseColorTexture | flags::kEmissiveTexture;
    const rv::gl::MaterialUniforms registered = rv::gl::makeMaterialUniforms(lamp, textures);

    const rv::gl::MaterialFactors factors = rv::gl::materialFactors(lamp);
    CHECK(factors.baseColor == lamp.baseColorFactor);
    CHECK(factors.metallic == lamp.metallicFactor);
    CHECK(factors.roughness == lamp.roughnessFactor);
    CHECK(factors.emissiveStrength == lamp.emissiveStrength);

    // Edited factors: the block of the material with those factors, its other fields and flags untouched.
    rv::gl::MaterialFactors edited = factors;
    edited.baseColor = glm::vec4(0.2f, 0.4f, 0.6f, 0.5f);
    edited.metallic = 0.9f;
    edited.roughness = 0.1f;
    edited.emissiveStrength = 0.0f; // switched off, then back on below: the emissive color comes back
    MaterialDesc expected = lamp;
    expected.baseColorFactor = edited.baseColor;
    expected.metallicFactor = edited.metallic;
    expected.roughnessFactor = edited.roughness;
    expected.emissiveStrength = edited.emissiveStrength;
    const rv::gl::MaterialUniforms off = rv::gl::withFactors(registered, edited, lamp.emissiveFactor);
    const rv::gl::MaterialUniforms reference = rv::gl::makeMaterialUniforms(expected, textures);
    CHECK(off.baseColorFactor == reference.baseColorFactor);
    CHECK(off.metallic == reference.metallic);
    CHECK(off.roughness == reference.roughness);
    CHECK(off.emissive == glm::vec3(0.0f));
    CHECK(off.emissiveStrength == 0.0f);
    CHECK(off.normalScale == registered.normalScale);
    CHECK(off.alphaCutoff == registered.alphaCutoff);
    CHECK(off.flags == registered.flags);

    edited.emissiveStrength = 4.0f;
    const rv::gl::MaterialUniforms brighter = rv::gl::withFactors(off, edited, lamp.emissiveFactor);
    CHECK(brighter.emissive == glm::vec3(4.0f, 2.32f, 1.0f));
    CHECK(brighter.emissiveStrength == 4.0f);

    // The same validation as makeMaterialUniforms: clamped, non-finite values take the defaults.
    rv::gl::MaterialFactors wild;
    wild.baseColor = glm::vec4(-1.0f, std::numeric_limits<float>::quiet_NaN(), 2.0f, 3.0f);
    wild.metallic = 7.0f;
    wild.roughness = -1.0f;
    wild.emissiveStrength = -2.0f;
    const rv::gl::MaterialUniforms clamped = rv::gl::withFactors(registered, wild, lamp.emissiveFactor);
    CHECK(clamped.baseColorFactor == glm::vec4(0.0f, 1.0f, 2.0f, 1.0f));
    CHECK(clamped.metallic == 1.0f);
    CHECK(clamped.roughness == 0.0f);
    CHECK(clamped.emissiveStrength == 0.0f);
    CHECK(clamped.emissive == glm::vec3(0.0f));
}

TEST_CASE("render/material_uniforms: a CRT screen carries its picture-tube parameters", "[render][materials][crt]") {
    MaterialDesc screen;
    screen.shading = rv::assets::ShadingModel::Unlit;
    // Without the effect the CRT fields stay zero and the flag is off.
    rv::gl::MaterialUniforms block = rv::gl::makeMaterialUniforms(screen, flags::kBaseColorTexture);
    CHECK((block.flags & flags::kCrtScreen) == 0u);
    CHECK(block.crtLineCount == 0.0f);
    CHECK(block.crtGlassReflection == 0.0f);

    rv::assets::CrtEffect crt;
    crt.lineCount = 240.0f;
    crt.scanlineStrength = 0.35f;
    crt.maskTriadCount = 320.0f;
    crt.maskStrength = 0.25f;
    crt.curvature = 0.06f;
    crt.vignette = 0.3f;
    crt.glassReflection = 0.8f;
    screen.crt = crt;
    block = rv::gl::makeMaterialUniforms(screen, flags::kBaseColorTexture);
    CHECK(block.flags == (flags::kBaseColorTexture | flags::kCrtScreen));
    CHECK(block.crtLineCount == 240.0f);
    CHECK(block.crtScanlineStrength == 0.35f);
    CHECK(block.crtMaskTriadCount == 320.0f);
    CHECK(block.crtMaskStrength == 0.25f);
    CHECK(block.crtCurvature == 0.06f);
    CHECK(block.crtVignette == 0.3f);
    CHECK(block.crtGlassReflection == 0.8f);
    CHECK(flags::kCrtScreen == 256u);

    // Values the shader cannot use never reach it: defaults for non-finite numbers, clamped ranges.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    screen.crt->lineCount = nan;
    screen.crt->maskTriadCount = -5.0f;
    screen.crt->scanlineStrength = 3.0f;
    screen.crt->curvature = 2.0f;
    screen.crt->glassReflection = nan;
    block = rv::gl::makeMaterialUniforms(screen, 0);
    CHECK(block.crtLineCount == rv::assets::CrtEffect{}.lineCount);
    CHECK(block.crtMaskTriadCount == 1.0f);
    CHECK(block.crtScanlineStrength == 1.0f);
    CHECK(block.crtCurvature == 0.5f);
    CHECK(block.crtGlassReflection == rv::assets::CrtEffect{}.glassReflection);

    // Live factor edits keep the CRT parameters.
    const rv::gl::MaterialUniforms edited =
        rv::gl::withFactors(block, rv::gl::materialFactors(screen), screen.emissiveFactor);
    CHECK(edited.crtCurvature == block.crtCurvature);
    CHECK(edited.flags == block.flags);
}

TEST_CASE("render/material_uniforms: a neon flickers now and then, never in a run's first seconds (I12 step 6)",
          "[render][materials][atmosphere]") {
    using rv::gl::EmissiveFlicker;
    for (std::uint32_t seed = 0; seed < 64; ++seed) {
        CAPTURE(seed);
        const EmissiveFlicker flicker{seed * 7919u, 1.0f, 0.6f};
        // Never before 4 s: a frame-limited run's screenshots (frame 30 at 60 Hz, 0.5 s) never catch a dip.
        for (float time = 0.0f; time < rv::gl::kFirstFlickerSeconds; time += 0.01f) {
            CHECK(rv::gl::emissiveFlickerFactor(flicker, time) == 1.0f);
        }
        // Then one dip of 80 to 120 ms in every slot of 8 s: about 1 to 1.5 % of the time dimmed, never darker than
        // 1 - depth.
        int dimmed = 0;
        int samples = 0;
        float lowest = 1.0f;
        for (float time = rv::gl::kFirstFlickerSeconds; time < rv::gl::kFirstFlickerSeconds + 80.0f; time += 0.002f) {
            const float factor = rv::gl::emissiveFlickerFactor(flicker, time);
            dimmed += factor < 1.0f ? 1 : 0;
            lowest = std::min(lowest, factor);
            ++samples;
        }
        const double share = static_cast<double>(dimmed) / samples;
        CHECK(share > 0.009);
        CHECK(share < 0.016);
        CHECK(lowest == Approx(0.4f));
    }
    // Rate or depth 0 (reduceMotion), or a time that is not finite: steady.
    CHECK(rv::gl::emissiveFlickerFactor(EmissiveFlicker{3u, 0.0f, 0.6f}, 30.0f) == 1.0f);
    CHECK(rv::gl::emissiveFlickerFactor(EmissiveFlicker{3u, 1.0f, 0.0f}, 30.0f) == 1.0f);
    CHECK(rv::gl::emissiveFlickerFactor(EmissiveFlicker{3u, 1.0f, 0.6f}, std::numeric_limits<float>::quiet_NaN()) ==
          1.0f);
}

TEST_CASE("render/material_uniforms: an LED blinks and a lava lamp breathes, from 4 s on (I12 step 8)",
          "[render][materials][life]") {
    using rv::gl::EmissiveFlicker;
    using rv::gl::FlickerMode;
    const EmissiveFlicker blink{1234u, 1.0f, 0.8f, FlickerMode::Blink};
    const EmissiveFlicker pulse{77u, 1.0f, 0.5f, FlickerMode::Pulse};
    for (float time = 0.0f; time < rv::gl::kFirstFlickerSeconds; time += 0.01f) {
        CHECK(rv::gl::emissiveFlickerFactor(blink, time) == 1.0f);
        CHECK(rv::gl::emissiveFlickerFactor(pulse, time) == 1.0f);
    }
    // The blink: on or off (1 - depth) in slots of 1/8 s, on in about 60 % of them, never anything between.
    int on = 0;
    int slots = 0;
    for (int slot = 0; slot < 4000; ++slot) {
        const float factor =
            rv::gl::emissiveFlickerFactor(blink, rv::gl::kFirstFlickerSeconds + (static_cast<float>(slot) + 0.5f) / 8.0f);
        const bool lit = factor == 1.0f;
        CHECK((lit || factor == Approx(0.2f)));
        on += lit ? 1 : 0;
        ++slots;
    }
    CHECK(static_cast<double>(on) / slots == Approx(0.6).margin(0.04));
    // The breath: one every 10 s, from 1 down to 1 - depth half-way and back.
    const float start = rv::gl::kFirstFlickerSeconds;
    CHECK(rv::gl::emissiveFlickerFactor(pulse, start + 5.0f) == Approx(0.5f));
    CHECK(rv::gl::emissiveFlickerFactor(pulse, start + 10.0f) == Approx(1.0f).margin(1e-4));
    CHECK(rv::gl::emissiveFlickerFactor(pulse, start + 2.5f) == Approx(0.75f).margin(1e-4));
    // reduceMotion: rate 0, steady.
    CHECK(rv::gl::emissiveFlickerFactor(EmissiveFlicker{5u, 0.0f, 0.8f, FlickerMode::Blink}, 30.0f) == 1.0f);
    CHECK(rv::gl::emissiveFlickerFactor(EmissiveFlicker{5u, 0.0f, 0.5f, FlickerMode::Pulse}, 30.0f) == 1.0f);
}
