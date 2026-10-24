#version 450 core
// Depth-only fragment stage of the meshes (rv::gl::SceneRenderer): the key light's shadow map and the depth pre-pass of
// a view write depth and nothing else. Vertex stage: pbr/pbr.vert, whose invariant gl_Position gives the pre-pass the
// very depth of the lit pass that follows it.
//
// RV_ALPHA_TEST (injected define): the program of alpha-masked materials samples the base color's alpha and discards
// below the cutoff like the lit pass (common/material.glsl), so cut-outs cast cut-out shadows and leave holes in the
// pre-pass. Without it main reads nothing, so the material block and textures are inactive: opaque batches draw
// without a material bound. The masked program also leaves out a face-out item's spine label (materialInstanceTest:
// spine-layer materials are alpha-masked for it).
#include "common/common.glsl"
#include "common/material.glsl"

layout(location = 3) in vec2 vUv;
layout(location = 4) flat in uvec4 vInstance;

void main() {
#if defined(RV_ALPHA_TEST)
    float alpha = materialBaseColor(vUv).a;
    materialAlphaTest(alpha);
    materialInstanceTest(vInstance);
#endif
}
