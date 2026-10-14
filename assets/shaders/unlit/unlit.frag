#version 450 core
// Unlit pass: one flat color per draw, no texture and no lighting. The framebuffer encodes the linear output to sRGB.
#include "common/common.glsl"

uniform vec4 uBaseColor; // linear RGBA: the material's base color factor

layout(location = 0) out vec4 oColor;

void main() {
    // Opaque pass (blending off): the stored alpha is 1 whatever the material's alpha, as the window shows it.
    oColor = vec4(uBaseColor.rgb, 1.0);
}
