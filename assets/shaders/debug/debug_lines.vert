#version 450 core
// Debug lines (rv::gl::DebugDraw): world-space line vertices (rv::scene::DebugVertex) with linear colors, depth-tested
// against the scene.
#include "common/common.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;

layout(location = 0) out vec4 vColor;

// Each vertex is pulled towards the eye by this fraction of its distance (2 mm at 1 m, 2 cm at 10 m), so that lines
// lying on surfaces (object bounds, socket axes on faces, the grid on the floor) win the depth test instead of
// flickering, at any distance and down to grazing angles of about 10 degrees. Surfaces closer than that in front of a
// line hide it.
const float RV_DEBUG_PULL = 0.002;

void main() {
    vColor = aColor;
    vec3 position = mix(aPosition, uCameraPosition, RV_DEBUG_PULL);
    gl_Position = uViewProjection * vec4(position, 1.0);
}
