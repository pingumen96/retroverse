#version 450 core
// Unlit_Emissive (style §25): screens, neon and the magenta fallback material. Output (common/scene_output.glsl): base
// color * texture * emissive strength as linear scene radiance into the HDR target, unaffected by lights and all of it
// emitted; the post pass (post/post.frag) exposes and tonemaps it like the lit surfaces, so that screens sit in the
// same tonal range as the room around them, and its bloom makes them glow softly. A CRT screen (RV_MATERIAL_CRT_SCREEN,
// common/crt.glsl) shows its picture through a picture tube, and its glass adds a faint reflection of the room's
// lights: reflected light, not emitted, so it never blooms. A layered material (I6 step 6: the arcade cabinets' screens,
// the collection's front layer through a picture tube) takes its picture from the layer its instance names, as the
// lit surfaces do (common/material.glsl). Vertex stage: pbr/pbr.vert.
#include "common/common.glsl"
#include "common/lights.glsl"
#include "common/brdf.glsl"
#include "common/material.glsl"
#include "common/crt.glsl"
#include "common/scene_output.glsl"

layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 3) in vec2 vUv;
layout(location = 4) flat in uvec4 vInstance; // the instance's attributes: front layer, spine layer, flags, pick id

void main() {
    bool crtScreen = materialHas(RV_MATERIAL_CRT_SCREEN);
    vec2 pictureUv = crtScreen ? crtPictureUv(vUv) : vUv;
    vec4 color = materialInstanceBaseColor(pictureUv, vInstance);
    if (crtScreen) {
        color.rgb *= crtPictureLight(pictureUv);
    }
    materialAlphaTest(color.a);
    materialInstanceTest(vInstance);
    vec3 emitted = color.rgb * uEmissiveStrength;
    vec3 radiance = emitted;
    if (crtScreen) {
        radiance += crtGlassReflection(vWorldPosition, vNormal);
    }
    writeSceneOutput(radiance, emitted, materialOutputAlpha(color.a));
}
