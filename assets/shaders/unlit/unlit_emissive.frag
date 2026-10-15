#version 450 core
// Unlit_Emissive (style §25): screens, neon and the magenta fallback material. Output (common/scene_output.glsl): base
// color * texture * emissive strength as linear scene radiance into the HDR target, unaffected by lights and all of it
// emitted; the post pass (post/post.frag) exposes and tonemaps it like the lit surfaces, so that screens sit in the
// same tonal range as the room around them, and its bloom makes them glow softly. Vertex stage: pbr/pbr.vert.
#include "common/common.glsl"
#include "common/material.glsl"
#include "common/scene_output.glsl"

layout(location = 3) in vec2 vUv;

void main() {
    vec4 color = materialBaseColor(vUv);
    materialAlphaTest(color.a);
    vec3 radiance = color.rgb * uEmissiveStrength;
    writeSceneOutput(radiance, radiance, materialOutputAlpha(color.a));
}
