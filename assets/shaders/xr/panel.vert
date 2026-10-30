#version 450 core
// A panel of the VR mode's UI (I10 step 5, rv::gl::PanelPass): the unit square [0, 1]^2 that uModel places in the world
// (x to the right of the picture, y up it), seen through uPanelViewProjection. Four vertices of a triangle strip made from
// gl_VertexID, no vertex buffer; the picture's top row (the UI's top, drawn upright into its texture) at y = 1. Its own
// uniforms, not the view's FrameUniforms (common.glsl's), which the pass leaves as they are.
#include "common/common.glsl"

uniform mat4 uPanelViewProjection;
uniform mat4 uPanelModel;

layout(location = 0) out vec2 vUv;

void main() {
    vec2 corner = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    vUv = corner;
    gl_Position = uPanelViewProjection * uPanelModel * vec4(corner, 0.0, 1.0);
}
