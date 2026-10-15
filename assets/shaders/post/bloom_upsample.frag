#version 450 core
// Bloom upsample (rv::gl::PostProcessor, common/post.glsl): the 3 x 3 tent filter of the next smaller bloom level
// (uSource), which the fixed-function blend mixes into this level: level = mix(level, tent(smaller level), scatter),
// with scatter the blend constant (rv::gl::bloomScatter, from the bloom radius).
#include "common/common.glsl"
#include "common/post.glsl"

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 oColor;

void main() {
    oColor = vec4(tentFilter(uSource, vUv), 1.0);
}
