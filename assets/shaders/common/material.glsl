// Material parameters at uniform binding 2 and material textures on units 0..4. Mirrors rv::gl::MaterialUniforms,
// rv::gl::material_flags and rv::gl::MaterialTextureSlot (src/render/include/rv/render/uniform_blocks.hpp), filled by
// rv::gl::MaterialRegistry from an rv::assets::MaterialDesc. Colors are linear.

layout(std140, binding = 2) uniform MaterialUniforms {
    vec4 uBaseColorFactor;     // RGBA
    vec3 uEmissive;            // emissive factor * emissive strength (lit materials)
    float uMetallic;
    float uRoughness;          // perceptual roughness
    float uNormalScale;
    float uOcclusionStrength;
    float uAlphaCutoff;        // with RV_MATERIAL_ALPHA_MASK
    float uEmissiveStrength;   // unlit materials: scales base color * texture
    uint uMaterialFlags;       // RV_MATERIAL_* bits
};

// Bits of uMaterialFlags. Texture bits mark the slots that hold a material texture; the others hold neutral defaults
// and are not sampled.
const uint RV_MATERIAL_BASE_COLOR_TEXTURE = 1u;
const uint RV_MATERIAL_METALLIC_ROUGHNESS_TEXTURE = 2u;
const uint RV_MATERIAL_NORMAL_TEXTURE = 4u;
const uint RV_MATERIAL_OCCLUSION_TEXTURE = 8u;
const uint RV_MATERIAL_EMISSIVE_TEXTURE = 16u;
const uint RV_MATERIAL_ALPHA_MASK = 32u;
const uint RV_MATERIAL_ALPHA_BLEND = 64u;
const uint RV_MATERIAL_DOUBLE_SIDED = 128u;

layout(binding = 0) uniform sampler2D uBaseColorTexture;         // sRGB, multiplies uBaseColorFactor
layout(binding = 1) uniform sampler2D uMetallicRoughnessTexture; // linear: G scales roughness, B scales metallic
layout(binding = 2) uniform sampler2D uNormalTexture;            // linear, tangent space
layout(binding = 3) uniform sampler2D uOcclusionTexture;         // linear, R channel
layout(binding = 4) uniform sampler2D uEmissiveTexture;          // sRGB, multiplies uEmissive

bool materialHas(uint flag) {
    return (uMaterialFlags & flag) != 0u;
}

// Base color times its texture.
vec4 materialBaseColor(vec2 uv) {
    vec4 color = uBaseColorFactor;
    if (materialHas(RV_MATERIAL_BASE_COLOR_TEXTURE)) {
        color *= texture(uBaseColorTexture, uv);
    }
    return color;
}

// Alpha mode: mask discards the fragments below the cutoff; only blend keeps the alpha in the output. Call it after
// the last texture fetch: implicit derivatives are undefined after a non-uniform discard (GLSL 4.50 §6.4).
void materialAlphaTest(float alpha) {
    if (materialHas(RV_MATERIAL_ALPHA_MASK) && alpha < uAlphaCutoff) {
        discard;
    }
}

float materialOutputAlpha(float alpha) {
    return materialHas(RV_MATERIAL_ALPHA_BLEND) ? clamp(alpha, 0.0, 1.0) : 1.0;
}
