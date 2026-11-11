#version 450 core
// A faint cone of light (atmosphere/cone.vert): the light scattered by the air inside it, thickest where the eye looks
// through the most of it (the side facing the eye), fading in from its apex and out towards its end, as bright as the
// air there scatters its light times the emitter's strength, half on each face (both are drawn); never above 0.15, far
// under the bloom's threshold. Added to the radiance; it emits nothing and adds no alpha.
#include "common/common.glsl"
#include "common/atmosphere.glsl"
#include "common/scene_output.glsl"

layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vAlong;
layout(location = 3) flat in uint vEmitter;

void main() {
    AtmosphereEmitter emitter = uAtmosphereEmitters[min(vEmitter, uint(RV_MAX_ATMOSPHERE_EMITTERS - 1))];
    vec3 toEye = safeNormalize(uCameraPosition - vWorldPosition, vec3(0.0, 0.0, 1.0));
    float facing = abs(dot(safeNormalize(vNormal, vec3(0.0, 1.0, 0.0)), toEye));
    float thickness = facing * sqrt(facing);
    float fade = smoothstep(0.0, 0.3, vAlong) * (1.0 - smoothstep(0.55, 1.0, vAlong));
    // The air scatters the light it crosses: the illuminance at that distance (radiance / d^2) over the chord the eye
    // looks through (about the cone's width there, 2 d tan), spread over the sphere's 4 pi: radiance tan / (2 pi d).
    float distance = max(emitter.length * vAlong, 0.3);
    vec3 scattered = emitter.radiance * (emitter.tanHalfAngle / (6.2831853 * distance));
    vec3 radiance = min(scattered * (0.5 * emitter.strength * thickness * fade), vec3(0.15));
    writeSceneOutput(radiance, vec3(0.0), 0.0);
}
