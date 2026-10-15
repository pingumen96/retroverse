#version 450 core
// Debug lines (rv::gl::DebugDraw): the vertex color as it is, a linear display value encoded by the sRGB target, so
// overlay colors stay exact (no lighting, exposure or tonemapping).
#include "common/common.glsl"

layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 oColor;

void main() {
    oColor = vColor;
}
