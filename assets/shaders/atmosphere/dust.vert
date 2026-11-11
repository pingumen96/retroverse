#version 450 core
// The dust of a view's atmosphere (I12 step 6): one camera-facing quad per mote, four vertices of a triangle strip,
// instance i the i-th mote of the emitters in order (AtmosphereEmitter::firstMote). A mote is where
// atmosphereMotePosition puts it at the block's time, lit by its emitter's light alone, at least uMinMotePixels wide
// (narrower, dimmer by the area it gained: the same light, no flicker between pixels). Fragment stage:
// atmosphere/dust.frag.
#include "common/common.glsl"
#include "common/atmosphere.glsl"

layout(location = 0) out vec2 vCorner;
layout(location = 1) flat out vec3 vRadiance;

void main() {
    uint mote = uint(gl_InstanceID);
    uint chosen = 0u;
    for (uint i = 0u; i < uAtmosphereEmitterCount; ++i) {
        AtmosphereEmitter candidate = uAtmosphereEmitters[i];
        if (candidate.kind != RV_ATMOSPHERE_LIGHT_CONE && mote >= candidate.firstMote &&
            mote < candidate.firstMote + candidate.motes) {
            chosen = i;
        }
    }
    AtmosphereEmitter emitter = uAtmosphereEmitters[chosen];
    uint index = mote - emitter.firstMote;
    float fade;
    vec3 world = atmosphereMotePosition(emitter, index, uAtmosphereTime, fade);
    // Motes catch the light as they tumble.
    float twinkle = 0.55 + 0.45 * sin(uAtmosphereTime * (0.8 + 1.7 * atmosphereUnit(emitter.seed, index, 7u)) +
                                      6.2831853 * atmosphereUnit(emitter.seed, index, 3u));
    // Dust scatters forwards: a mote between the eye and its light glows, one with the light behind the eye barely
    // shows (Henyey-Greenstein, g 0.6, 1 for an even scatter).
    vec3 fromLight = safeNormalize(world - emitter.lightPosition, emitter.axis);
    vec3 toEye = safeNormalize(uCameraPosition - world, -emitter.axis);
    float g = 0.6;
    float phase = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * dot(fromLight, toEye), 1.5);
    vec3 radiance = atmosphereLightAt(emitter, world) * (uMoteBrightness * fade * twinkle * phase);

    vec4 viewPosition = uView * vec4(world, 1.0);
    float depth = max(-viewPosition.z, 1e-3);
    float pixels = uMoteSize * uProjection[1][1] * 0.5 * uAtmosphereViewport.y / depth;
    float drawn = max(pixels, uMinMotePixels);
    radiance *= (pixels * pixels) / (drawn * drawn);
    float size = uMoteSize * drawn / max(pixels, 1e-6);
    vec2 corner = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0, (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
    viewPosition.xy += corner * (0.5 * size);
    gl_Position = uProjection * viewPosition;
    vCorner = corner;
    vRadiance = radiance;
}
