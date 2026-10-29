// Material parameters at uniform binding 2, material textures on units 0..4 and the collection's layered base color on
// unit 12. Mirrors rv::gl::MaterialUniforms, rv::gl::material_flags, rv::gl::MaterialTextureSlot and
// rv::gl::CollectionTextureUnit (src/render/include/rv/render/uniform_blocks.hpp), filled by rv::gl::MaterialRegistry
// from an rv::assets::MaterialDesc. Colors are linear.

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
    // The picture-tube look of an unlit screen, with RV_MATERIAL_CRT_SCREEN (common/crt.glsl); zero otherwise.
    float uCrtLineCount;         // scanlines over the picture's height
    float uCrtMaskTriadCount;    // phosphor triads across the picture's width
    float uCrtScanlineStrength;  // [0, 1]
    float uCrtMaskStrength;      // [0, 1]
    float uCrtCurvature;         // [0, 0.5]
    float uCrtVignette;          // [0, 1]
    float uCrtGlassReflection;   // [0, 1]
    // The part of a screen its picture fills (I5 step 4), with RV_MATERIAL_PICTURE_RECT: its corner (xy) and size (zw)
    // in the screen's texture coordinates; the tube's off-black around it (a letterbox or a pillarbox, never a
    // stretch).
    vec4 uPictureRect;
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
const uint RV_MATERIAL_CRT_SCREEN = 256u;
// The base color also multiplies the layer of uLayeredBaseColorTexture that the instance record names: its spine layer
// with RV_MATERIAL_SPINE_LAYER, else its front layer (materialInstanceBaseColor).
const uint RV_MATERIAL_LAYERED_BASE_COLOR = 512u;
const uint RV_MATERIAL_SPINE_LAYER = 1024u;
// The picture fills uPictureRect of the screen, the bars around it (materialPictureUv, materialPictureCoverage).
const uint RV_MATERIAL_PICTURE_RECT = 2048u;

layout(binding = 0) uniform sampler2D uBaseColorTexture;         // sRGB, multiplies uBaseColorFactor
layout(binding = 1) uniform sampler2D uMetallicRoughnessTexture; // linear: G scales roughness, B scales metallic
layout(binding = 2) uniform sampler2D uNormalTexture;            // linear, tangent space
layout(binding = 3) uniform sampler2D uOcclusionTexture;         // linear, R channel
layout(binding = 4) uniform sampler2D uEmissiveTexture;          // sRGB, multiplies uEmissive
// The collection's fronts or spines (rv::gl::CollectionArrays), one picture per layer, sRGB; layer 0 is the pending
// picture an item shows until its own arrives.
layout(binding = 12) uniform sampler2DArray uLayeredBaseColorTexture;

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

// Base color of the instance whose record holds `instance` (front layer, spine layer, flags, pick id: vInstance of
// common/surface.glsl): materialBaseColor, times the layer it names for a layered material. A layer beyond the array's
// last reads the last (GL clamps the layer coordinate).
vec4 materialInstanceBaseColor(vec2 uv, uvec4 instance) {
    vec4 color = materialBaseColor(uv);
    if (materialHas(RV_MATERIAL_LAYERED_BASE_COLOR)) {
        uint layer = materialHas(RV_MATERIAL_SPINE_LAYER) ? instance.y : instance.x;
        color *= texture(uLayeredBaseColorTexture, vec3(uv, float(layer)));
    }
    return color;
}

// An instance flagged RV_INSTANCE_NO_SPINE_LABEL (a face-out item, I6 step 4) draws no spine label: the fragments of a
// spine-layer material are discarded (the shell lies beneath). Spine-layer materials are alpha-masked
// (rv::gl::MaterialRegistry::addLayered), so that the depth passes and the hover outline bind them and call this too.
// Call it after the last texture fetch, like materialAlphaTest.
void materialInstanceTest(uvec4 instance) {
    if (materialHas(RV_MATERIAL_SPINE_LAYER) && (instance.z & RV_INSTANCE_NO_SPINE_LABEL) != 0u) {
        discard;
    }
}

// Alpha mode: mask discards the fragments below the cutoff; only blend keeps the alpha in the output. Call it after
// the last texture fetch: implicit derivatives are undefined after a non-uniform discard (GLSL 4.50 §6.4).
void materialAlphaTest(float alpha) {
    if (materialHas(RV_MATERIAL_ALPHA_MASK) && alpha < uAlphaCutoff) {
        discard;
    }
}

// The picture's texture coordinates at the screen's `uv` with RV_MATERIAL_PICTURE_RECT: uPictureRect mapped onto
// [0, 1].
vec2 materialPictureUv(vec2 uv) {
    return (uv - uPictureRect.xy) / uPictureRect.zw;
}

// How much of the pixel at the screen's `uv` the picture of uPictureRect covers (anti-aliased over a pixel): 1 inside,
// 0 in the bars around it. Uses derivatives: call it in uniform control flow, before any discard.
float materialPictureCoverage(vec2 uv) {
    vec2 pixel = max(fwidth(uv), vec2(1e-6));
    vec2 low = clamp((uv - uPictureRect.xy) / pixel + 0.5, 0.0, 1.0);
    vec2 high = clamp((uPictureRect.xy + uPictureRect.zw - uv) / pixel + 0.5, 0.0, 1.0);
    return low.x * low.y * high.x * high.y;
}

float materialOutputAlpha(float alpha) {
    return materialHas(RV_MATERIAL_ALPHA_BLEND) ? clamp(alpha, 0.0, 1.0) : 1.0;
}
