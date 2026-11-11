// The atmosphere of a view (I12 step 6, ADR-0021 §4): dust motes in a light's cone or pool and faint cones of light,
// drawn additively into the HDR target's radiance after the blended batches (rv::gl::AtmospherePass). Mirrors
// rv::gl::AtmosphereUniforms (src/render/include/rv/render/uniform_blocks.hpp) member for member (std140): change both
// together. Include after common/common.glsl.

#define RV_MAX_ATMOSPHERE_EMITTERS 16
const uint RV_ATMOSPHERE_DUST_CONE = 0u;
const uint RV_ATMOSPHERE_DUST_POOL = 1u;
const uint RV_ATMOSPHERE_LIGHT_CONE = 2u;

struct AtmosphereEmitter {
    vec3 apex;
    float length;
    vec3 axis;
    float tanHalfAngle;
    vec3 lightPosition;
    float range;
    vec3 lightDirection;
    float radius;
    vec3 radiance;
    float strength;
    float spotScale;
    float spotOffset;
    uint kind;
    uint firstMote;
    uint motes;
    uint seed;
};

layout(std140, binding = 6) uniform AtmosphereUniforms {
    AtmosphereEmitter uAtmosphereEmitters[RV_MAX_ATMOSPHERE_EMITTERS];
    vec2 uAtmosphereViewport;    // pixels
    float uAtmosphereTime;       // seconds the motes have drifted (0: frozen)
    float uMoteSize;             // meters
    float uMoteBrightness;       // share of the light's radiance at the mote sent towards the eye
    float uMinMotePixels;
    uint uAtmosphereEmitterCount;
};

// lowbias32, as rv::scene::atmosphereHash.
uint atmosphereHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float atmosphereUnit(uint seed, uint index, uint k) {
    return float(atmosphereHash(seed ^ atmosphereHash(index * 8u + k))) * (1.0 / 4294967296.0);
}

// Where mote `index` of `emitter` is at `time`, and the share of its brightness its place along the axis leaves (0 at
// the ends, where it wraps): rv::scene::motePosition, formula for formula.
vec3 atmosphereMotePosition(AtmosphereEmitter emitter, uint index, float time, out float fade) {
    float r[8];
    for (uint k = 0u; k < 8u; ++k) {
        r[k] = atmosphereUnit(emitter.seed, index, k);
    }
    const float tau = 6.2831853;
    float speed = (0.0015 + 0.0025 * r[4]) / emitter.length;
    float t = fract(r[0] + time * speed);
    fade = smoothstep(0.0, 0.08, t) * (1.0 - smoothstep(0.85, 1.0, t));
    bool pool = emitter.kind == RV_ATMOSPHERE_DUST_POOL;
    float along = pool ? emitter.length * t : emitter.length * (0.06 + 0.94 * t);
    float reach = pool ? emitter.radius : along * emitter.tanHalfAngle;
    float offset = sqrt(r[1]) * 0.92 * reach;
    float angle = tau * r[2];
    vec3 side = abs(emitter.axis.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 u = normalize(cross(emitter.axis, side));
    vec3 v = cross(emitter.axis, u);
    vec3 wobble = 0.03 * vec3(sin(time * (0.31 + 0.40 * r[5]) + tau * r[1]),
                              sin(time * (0.23 + 0.30 * r[6]) + tau * r[2]),
                              sin(time * (0.27 + 0.35 * r[7]) + tau * r[3]));
    return emitter.apex + emitter.axis * along + (u * cos(angle) + v * sin(angle)) * offset + wobble;
}

// The radiance of the emitter's light reaching `point` (common/lights.glsl's falloff and cone factor).
vec3 atmosphereLightAt(AtmosphereEmitter emitter, vec3 point) {
    vec3 toPoint = point - emitter.lightPosition;
    float distanceSquared = max(dot(toPoint, toPoint), 1e-4);
    float ratioSquared = distanceSquared / (emitter.range * emitter.range);
    float window = clamp(1.0 - ratioSquared * ratioSquared, 0.0, 1.0);
    float attenuation = window * window / distanceSquared;
    float t = clamp(dot(emitter.lightDirection, toPoint * inversesqrt(distanceSquared)) * emitter.spotScale +
                        emitter.spotOffset,
                    0.0, 1.0);
    return emitter.radiance * attenuation * t * t * (3.0 - 2.0 * t);
}
