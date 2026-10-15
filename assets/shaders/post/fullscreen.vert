#version 450 core
// Fullscreen triangle of the post-processing passes (rv::gl::PostProcessor): its three vertices come from gl_VertexID,
// so the passes draw without a vertex buffer (an empty vertex array is bound). The counter-clockwise triangle
// (-1, -1), (3, -1), (-1, 3) covers clip space [-1, 1]^2; vUv runs from (0, 0) at the bottom-left of the target to
// (1, 1) at its top-right, matching the rows of render target textures (bottom row first).
#include "common/common.glsl"

layout(location = 0) out vec2 vUv;

void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); // (0, 0), (2, 0), (0, 2)
    vUv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
