// Scene lights and environment at uniform binding 1. Mirrors rv::gl::LightsUniforms and
// rv::gl::PunctualLightUniforms member for member (std140; src/render/include/rv/render/uniform_blocks.hpp), filled
// by rv::gl::packLights. Light contract: rv::scene::Light (src/scene/include/rv/scene/light.hpp).
// Needs common/common.glsl.

#define RV_MAX_PUNCTUAL_LIGHTS 8

struct PunctualLight {
    vec3 position;    // world, meters
    float range;      // meters: the light contributes nothing at or beyond it
    vec3 color;       // linear RGB
    float intensity;  // scales color
    vec3 direction;   // unit vector the light travels along (spot axis)
    float spotScale;  // cone: t = clamp(dot(direction, -L) * spotScale + spotOffset, 0, 1);
    float spotOffset; //       point lights have scale 0 and offset 1 (no cone)
};

layout(std140, binding = 1) uniform LightsUniforms {
    PunctualLight uPunctualLights[RV_MAX_PUNCTUAL_LIGHTS]; // the first uPunctualLightCount are used
    vec3 uDirectionalDirection;  // unit vector the directional light travels along
    float uDirectionalIntensity; // 0 when the scene has no directional light
    vec3 uDirectionalColor;      // linear RGB
    int uPunctualLightCount;
    vec3 uAmbientSky;            // linear RGB of the upper hemisphere (+Y)
    float uAmbientIntensity;
    vec3 uAmbientGround;         // linear RGB of the lower hemisphere
    float uExposure;             // multiplies the linear color before tonemapping
};

// Inverse-square falloff times a window that reaches exactly 0 at the range:
//   window(d) = clamp(1 - (d / range)^4, 0, 1)^2, attenuation = window(d) / max(d^2, 1e-4).
float distanceAttenuation(float distanceSquared, float range) {
    float ratioSquared = distanceSquared / (range * range);
    float window = clamp(1.0 - ratioSquared * ratioSquared, 0.0, 1.0);
    return window * window / max(distanceSquared, 1e-4);
}

// Spot cone factor for the unit vector L from the surface towards the light: 1 inside the inner cone, 0 outside the
// outer cone, smoothstep in between (1 everywhere for point lights).
float spotFactor(PunctualLight light, vec3 L) {
    float t = clamp(dot(light.direction, -L) * light.spotScale + light.spotOffset, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Hemispherical ambient radiance arriving along the unit direction `n`: ground color below, sky color above.
vec3 hemisphereAmbient(vec3 n) {
    return mix(uAmbientGround, uAmbientSky, n.y * 0.5 + 0.5) * uAmbientIntensity;
}
