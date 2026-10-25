#version 450 core
// The hover outline's mark (rv::gl::OutlinePass, ADR-0012 §3): the highlighted instances drawn once more after the
// lit pass, into the stencil of the HDR target only. It writes no colour; the pass's state (the depth test GL_LEQUAL
// against the scene, without depth writes; the stencil set on every fragment that passes) does the rest. Vertex
// stage: pbr/pbr.vert, whose invariant gl_Position meets the scene's depth exactly, so the stencil holds the visible part
// of the silhouette. Only the fragments of an instance flagged RV_INSTANCE_HIGHLIGHTED are kept, so a draw of other
// instances marks nothing.
//
// RV_ALPHA_TEST (injected define): alpha-masked batches discard below their cutoff like the lit pass, so cut-outs stay
// out of the mark, and so does a face-out item's spine label (materialInstanceTest).
#include "common/common.glsl"
#include "common/material.glsl"

layout(location = 3) in vec2 vUv;
layout(location = 4) flat in uvec4 vInstance;

void main() {
#if defined(RV_ALPHA_TEST)
    float alpha = materialBaseColor(vUv).a;
#endif
    if ((vInstance.z & RV_INSTANCE_HIGHLIGHTED) == 0u) {
        discard;
    }
#if defined(RV_ALPHA_TEST)
    materialAlphaTest(alpha);
    materialInstanceTest(vInstance);
#endif
}
