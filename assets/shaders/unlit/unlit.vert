#version 450 core
// Unlit pass: transforms positions only. Every draw sets uModel (one object instance).
#include "common/common.glsl"

layout(location = 0) in vec3 aPosition;

uniform mat4 uModel; // local -> world

void main() {
    gl_Position = uViewProjection * (uModel * vec4(aPosition, 1.0));
}
