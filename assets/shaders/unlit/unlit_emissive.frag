#version 450 core
// Unlit_Emissive (style §25): screens, neon and the magenta fallback material. Base color * texture * emissive
// strength, unaffected by lights, then the exposure and ACES fitted transform of lit surfaces (common/tonemap.glsl), so
// that screens sit in the same tonal range as the room around them. Vertex stage: pbr/pbr.vert.
//
// RV_UNLIT_LINEAR_OUTPUT (injected define) skips exposure and tonemapping and writes the linear value as it is: the
// sRGB self-check draws linear 0.5 with it and expects the framebuffer to store sRGB 188.
#include "common/common.glsl"
#include "common/lights.glsl"
#include "common/tonemap.glsl"
#include "common/material.glsl"

layout(location = 3) in vec2 vUv;

layout(location = 0) out vec4 oColor;

void main() {
    vec4 color = materialBaseColor(vUv);
    materialAlphaTest(color.a);
    vec3 radiance = color.rgb * uEmissiveStrength;
#if defined(RV_UNLIT_LINEAR_OUTPUT)
    oColor = vec4(radiance, materialOutputAlpha(color.a));
#else
    oColor = vec4(tonemap(radiance, uExposure), materialOutputAlpha(color.a));
#endif
}
