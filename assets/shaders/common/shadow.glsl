// Shadow of the key light (rv::gl::SceneRenderer): the ShadowUniforms block at uniform binding 4 and the shadow map on
// texture unit 8, read by the lit shader (pbr/pbr.frag). Mirrors rv::gl::ShadowUniforms,
// rv::gl::kDirectionalShadowLight and rv::gl::SceneTextureUnit::ShadowMap
// (src/render/include/rv/render/uniform_blocks.hpp), filled per view by rv::gl::makeShadowUniforms. Needs
// common/common.glsl.
//
// The shadow map holds the depth of the shadow casters seen from the key light, the first scene light that casts
// shadows: through a perspective projection for a spot light, an orthographic one fitted to the casters for a
// directional light (point lights cast none). A surface point is in shadow where a caster is nearer to the light. Two
// biases keep surfaces from shadowing themselves (acne) while shadows stay attached to their casters (no
// peter-panning): the casters' depth is pushed back by their slope when the map is drawn (glPolygonOffset,
// rv::gl::ShadowSettings), and receivers are looked up from their position moved along their normal by about one and a
// half texels of the map (the normal offset), a few millimeters at room scale.

const int RV_NO_SHADOW_LIGHT = -1;
// uShadowLightIndex of the directional light; the point and spot lights are 0 .. RV_MAX_PUNCTUAL_LIGHTS - 1.
const int RV_DIRECTIONAL_SHADOW_LIGHT = 8;

layout(std140, binding = 4) uniform ShadowUniforms {
    mat4 uShadowViewProjection; // world -> clip space of the shadow map
    vec3 uShadowLightPosition;  // eye of a perspective map: the normal offset grows with the distance from it
    int uShadowLightIndex;      // RV_NO_SHADOW_LIGHT, a punctual light index or RV_DIRECTIONAL_SHADOW_LIGHT
    vec2 uShadowNormalOffset;   // meters along the normal: x + y * distance from uShadowLightPosition
    float uShadowStrength;      // [0, 1]: share of the key light's radiance its shadow takes away
    float uShadowTexelSize;     // one texel of the map in texture coordinates: 1 / resolution
};

// Depth of the casters, compared in hardware (GL_COMPARE_REF_TO_TEXTURE with GL_LEQUAL: 1 where the reference is not
// behind a caster) and filtered bilinearly, so that every fetch is a 2 x 2 percentage-closer filter. It is clamped to a
// border of depth 1: nothing is in shadow outside the map.
layout(binding = 8) uniform sampler2DShadow uShadowMap;

// Share of the key light reaching `worldPosition` ([0, 1], 1: lit), for a surface with the unit geometric normal
// `normal` (facing the side being shaded): a 3 x 3 tent (1 2 1 / 2 4 2 / 1 2 1) of hardware-filtered fetches one texel
// apart, a smooth filter over 4 x 4 texels, mixed towards 1 by the strength. Points outside the map, or behind a spot
// light, are lit; points beyond the far plane are compared as at the far plane, so only casters darken them. Explicit
// level-of-detail fetches: valid after an alpha test may have discarded.
float keyLightVisibility(vec3 worldPosition, vec3 normal) {
    float offset = uShadowNormalOffset.x + uShadowNormalOffset.y * distance(worldPosition, uShadowLightPosition);
    vec4 clip = uShadowViewProjection * vec4(worldPosition + normal * offset, 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    vec3 coord = clip.xyz / clip.w * 0.5 + 0.5;
    if (any(lessThan(coord.xy, vec2(0.0))) || any(greaterThan(coord.xy, vec2(1.0)))) {
        return 1.0;
    }
    coord.z = min(coord.z, 1.0);
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float weight = float((2 - abs(x)) * (2 - abs(y)));
            vec2 uv = coord.xy + vec2(float(x), float(y)) * uShadowTexelSize;
            visibility += weight * textureLod(uShadowMap, vec3(uv, coord.z), 0.0);
        }
    }
    return mix(1.0, visibility / 16.0, uShadowStrength);
}
