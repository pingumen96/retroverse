#version 450 core
// Unlit_Emissive (style §25): screens, neon and the magenta fallback material. Output: base color * texture * emissive
// strength as linear scene radiance into the HDR target, unaffected by lights; the post pass (post/post.frag) exposes
// and tonemaps it like the lit surfaces, so that screens sit in the same tonal range as the room around them, and its
// bloom makes the brightest ones glow. Vertex stage: pbr/pbr.vert.
#include "common/common.glsl"
#include "common/material.glsl"

layout(location = 3) in vec2 vUv;

layout(location = 0) out vec4 oColor;

void main() {
    vec4 color = materialBaseColor(vUv);
    materialAlphaTest(color.a);
    oColor = vec4(color.rgb * uEmissiveStrength, materialOutputAlpha(color.a));
}
