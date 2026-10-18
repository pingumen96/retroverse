#version 450 core
// The hover outline's edge (rv::gl::OutlinePass, ADR-0012 §3): after the anti-aliasing, a fullscreen triangle scissored
// around the highlighted instances (post/fullscreen.vert) marks every pixel outside their silhouettes that lies within
// uOutlineWidth pixels of one inside: a pixel inside the stencil the mark wrote (outline/outline_mark.frag) at an offset
// (dx, dy) with dx^2 + dy^2 <= width^2 (rv::scene::outlineCovers, the same test on the CPU). Those pixels get the
// outline's colour, which the pass writes with GL_FRAMEBUFFER_SRGB off, so the output stores its sRGB bytes exactly;
// every other pixel is left as the post-processing wrote it (discarded). Nothing is blended or filtered: the outline is
// the same set of pixels for the same silhouette, frame after frame.
#include "common/common.glsl"

// The stencil of the HDR target through its stencil-index view (rv::gl::OutlineTextureUnit::Stencil): non-zero where
// the mark drew. Read with texelFetch only (an integer texture, nearest).
layout(binding = 13) uniform usampler2D uOutlineStencil;
// The outline's colour, sRGB-encoded in [0, 1].
uniform vec3 uOutlineColor;
// Pixels (rv::scene::outlineWidthPixels: 2 at 1080 output rows).
uniform float uOutlineWidth;

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 oColor;

// rv::scene::kOutlineMaxReach and the tolerance of rv::scene::outlineCovers, in squared pixels.
const int RV_OUTLINE_MAX_REACH = 8;
const float RV_OUTLINE_TOLERANCE = 0.001;

bool marked(ivec2 pixel, ivec2 size) {
    if (any(lessThan(pixel, ivec2(0))) || any(greaterThanEqual(pixel, size))) {
        return false;
    }
    return texelFetch(uOutlineStencil, pixel, 0).r != 0u;
}

void main() {
    ivec2 size = textureSize(uOutlineStencil, 0);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if (marked(pixel, size)) {
        discard; // the item itself
    }
    int reach = min(int(floor(uOutlineWidth + RV_OUTLINE_TOLERANCE)), RV_OUTLINE_MAX_REACH);
    float limit = uOutlineWidth * uOutlineWidth + RV_OUTLINE_TOLERANCE;
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            if (float(dx * dx + dy * dy) <= limit && marked(pixel + ivec2(dx, dy), size)) {
                oColor = vec4(uOutlineColor, 1.0);
                return;
            }
        }
    }
    discard;
}
