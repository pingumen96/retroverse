// Scene lights and ambient at uniform binding 1. Mirrors rv::gl::LightsUniforms and rv::gl::PunctualLightUniforms
// member for member (std140; src/render/include/rv/render/uniform_blocks.hpp), filled by rv::gl::packLights. Light
// contract: rv::scene::Light (src/scene/include/rv/scene/light.hpp). Needs common/common.glsl.

#define RV_MAX_PUNCTUAL_LIGHTS 16

struct PunctualLight {
    vec3 position;    // world, meters
    float range;      // meters: the light contributes nothing at or beyond it
    vec3 color;       // linear RGB
    float intensity;  // scales color
    vec3 direction;   // unit vector the light travels along (spot axis)
    float spotScale;  // cone: t = clamp(dot(direction, -L) * spotScale + spotOffset, 0, 1);
    float spotOffset; //       point lights have scale 0 and offset 1 (no cone)
    int portalAxis;    // the portal it is clipped to (I6 step 1): 0 x, 1 y, 2 z, -1 none
    float portalPlane; // the portal's plane: coordinate portalAxis equals this
    vec4 portalRect;   // its rectangle over the other two axes in order (y z, x z, x y): min u, min v, max u, max v
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
    vec3 uAmbientFill;           // linear RGB of the even fill, whatever the normal (I6 step 1)
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

// How far a portal's plane reaches (meters): the wall faces in it (a hall's wall around a doorway) count as the light's
// side.
#define RV_PORTAL_MARGIN 0.01
// The size a light is taken to have for the soft edge of the light a portal lets through (meters): the edge's width is
// this times the fragment's distance from the portal's plane over the light's, at least RV_PORTAL_MIN_EDGE.
#define RV_PORTAL_LIGHT_SIZE 0.25
#define RV_PORTAL_MIN_EDGE 0.02

// How much of `light` reaches `position` as far as its portal goes (I6 step 1): all of it on the light's side of the
// portal's plane (or within RV_PORTAL_MARGIN of it, or without a portal); beyond it, the light whose straight line to
// the position crosses the plane within the portal's rectangle, with a soft edge (the penumbra of a light of
// RV_PORTAL_LIGHT_SIZE through the opening), so that a light shines through a doorway and not through the wall beside
// it.
float portalVisibility(PunctualLight light, vec3 position) {
    if (light.portalAxis < 0) {
        return 1.0;
    }
    int axis = light.portalAxis;
    float lightSide = light.position[axis] - light.portalPlane;
    float positionSide = position[axis] - light.portalPlane;
    if (lightSide * positionSide >= 0.0 || abs(positionSide) <= RV_PORTAL_MARGIN) {
        return 1.0;
    }
    vec3 crossing = mix(light.position, position, lightSide / (lightSide - positionSide));
    vec2 onPlane = axis == 0 ? crossing.yz : (axis == 1 ? crossing.xz : crossing.xy);
    // The signed distance from the rectangle's edge, negative inside it.
    vec2 outside = max(light.portalRect.xy - onPlane, onPlane - light.portalRect.zw);
    float distanceOut = max(outside.x, outside.y);
    float edge = max(RV_PORTAL_LIGHT_SIZE * abs(positionSide) / max(abs(lightSide), 1e-3), RV_PORTAL_MIN_EDGE);
    return 1.0 - smoothstep(-0.5 * edge, 0.5 * edge, distanceOut);
}

// Radiance that punctual light `index` brings to `position` (color * intensity * attenuation * cone factor), with the
// unit direction from the position towards the light in `L`, times what its portal lets through (portalVisibility);
// zero at or beyond its range and where its portal stops it, where `L` is left undefined.
vec3 punctualLightRadiance(int index, vec3 position, out vec3 L) {
    PunctualLight light = uPunctualLights[index];
    vec3 toLight = light.position - position;
    float distanceSquared = dot(toLight, toLight);
    L = vec3(0.0, 0.0, 1.0);
    if (distanceSquared >= light.range * light.range) {
        return vec3(0.0); // the window is exactly 0 from the range on
    }
    float through = portalVisibility(light, position);
    if (through <= 0.0) {
        return vec3(0.0); // beyond a portal, outside its opening
    }
    L = toLight * inversesqrt(max(distanceSquared, 1e-8));
    float attenuation = distanceAttenuation(distanceSquared, light.range) * spotFactor(light, L) * through;
    return light.color * (light.intensity * attenuation);
}

// Ambient radiance arriving along the unit direction `n`: the hemisphere's ground color below, sky color above.
vec3 hemisphereAmbient(vec3 n) {
    return mix(uAmbientGround, uAmbientSky, n.y * 0.5 + 0.5) * uAmbientIntensity;
}

// The diffuse ambient around the normal `n`: the hemisphere's plus the even fill (I6 step 1). The fill is the light the
// lamps scatter around a room, diffuse only: glossy surfaces reflect the hemisphere alone, so that the fill lifts the
// dark walls and ceilings without a sheen on every floor and screen.
vec3 diffuseAmbient(vec3 n) {
    return hemisphereAmbient(n) + uAmbientFill;
}
