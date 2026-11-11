#version 450 core
// The light cones of a view's atmosphere (I12 step 6): instance i draws emitter i when it is a light cone (any other is
// clipped away), as RV_CONE_SEGMENTS x RV_CONE_RINGS quads of its side from its apex down its axis, no vertex buffer.
// Fragment stage: atmosphere/cone.frag.
#include "common/common.glsl"
#include "common/atmosphere.glsl"

#define RV_CONE_SEGMENTS 24u
#define RV_CONE_RINGS 8u

layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vAlong;
layout(location = 3) flat out uint vEmitter;

void main() {
    uint emitterIndex = uint(gl_InstanceID);
    AtmosphereEmitter emitter = uAtmosphereEmitters[min(emitterIndex, uint(RV_MAX_ATMOSPHERE_EMITTERS - 1))];
    vEmitter = emitterIndex;
    if (emitterIndex >= uAtmosphereEmitterCount || emitter.kind != RV_ATMOSPHERE_LIGHT_CONE) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside the clip volume: nothing drawn
        vWorldPosition = vec3(0.0);
        vNormal = vec3(0.0, 1.0, 0.0);
        vAlong = 0.0;
        return;
    }
    const uvec2 corners[6] = uvec2[6](uvec2(0u, 0u), uvec2(1u, 0u), uvec2(1u, 1u), uvec2(0u, 0u), uvec2(1u, 1u),
                                      uvec2(0u, 1u));
    uint quad = uint(gl_VertexID) / 6u;
    uvec2 corner = corners[uint(gl_VertexID) % 6u];
    uint segment = quad % RV_CONE_SEGMENTS + corner.x;
    uint ring = quad / RV_CONE_SEGMENTS + corner.y;
    float along = float(ring) / float(RV_CONE_RINGS);
    float angle = 6.2831853 * float(segment) / float(RV_CONE_SEGMENTS);
    vec3 side = abs(emitter.axis.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 u = normalize(cross(emitter.axis, side));
    vec3 v = cross(emitter.axis, u);
    vec3 radial = u * cos(angle) + v * sin(angle);
    float distance = emitter.length * along;
    vec3 world = emitter.apex + emitter.axis * distance + radial * (distance * emitter.tanHalfAngle);
    vWorldPosition = world;
    vNormal = normalize(radial - emitter.axis * emitter.tanHalfAngle);
    vAlong = along;
    gl_Position = uViewProjection * vec4(world, 1.0);
}
