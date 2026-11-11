#version 450 core
// A mote (atmosphere/dust.vert): a soft round speck added to the radiance; it emits nothing (oEmission 0: dust never
// blooms of itself) and adds no alpha.
#include "common/common.glsl"
#include "common/scene_output.glsl"

layout(location = 0) in vec2 vCorner;
layout(location = 1) flat in vec3 vRadiance;

void main() {
    float r2 = dot(vCorner, vCorner);
    if (r2 > 1.0) {
        discard;
    }
    float falloff = (exp(-3.0 * r2) - exp(-3.0)) / (1.0 - exp(-3.0));
    writeSceneOutput(vRadiance * falloff, vec3(0.0), 0.0);
}
