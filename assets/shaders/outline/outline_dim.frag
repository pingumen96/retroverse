#version 450 core
// The inspection's dimming (rv::gl::OutlinePass, ADR-0012 §4, I3 step 13b): after the anti-aliasing, a fullscreen
// triangle (post/fullscreen.vert) over every pixel outside the silhouettes the mark wrote into the stencil
// (outline/outline_mark.frag: the inspected item), drawn as black at uDimShare's opacity and blended with
// GL_FRAMEBUFFER_SRGB on, so that the scene behind the item keeps 1 - uDimShare of its linear light (one stop: 0.5).
// The item itself is left as the post-processing wrote it (discarded).
#include "common/common.glsl"

// The stencil of the HDR target through its stencil-index view (rv::gl::OutlineTextureUnit::Stencil): non-zero where
// the mark drew. Read with texelFetch only (an integer texture, nearest).
layout(binding = 13) uniform usampler2D uOutlineStencil;
// [0, 1]: the share of the light taken away.
uniform float uDimShare;

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 oColor;

void main() {
    if (texelFetch(uOutlineStencil, ivec2(gl_FragCoord.xy), 0).r != 0u) {
        discard; // the item itself
    }
    oColor = vec4(0.0, 0.0, 0.0, uDimShare);
}
