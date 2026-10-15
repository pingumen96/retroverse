#version 450 core
// Bloom downsample (rv::gl::PostProcessor, common/post.glsl): the 13-tap filter of Jimenez 2014 from uSource (the HDR
// scene for the first level, else the previous level) into a level of half its size. Each output texel averages five
// overlapping 2 x 2 boxes of bilinear taps, one and two source texels around its center.
//
// RV_BLOOM_KARIS (injected define, the first level only): each tap of the HDR scene is made safe to filter
// (sanitizeRadiance) and reduced to the light that blooms (bloomExcess), then the boxes are combined with the Karis
// average: each is weighted by 1 / (1 + its exposed luminance) and the weights are normalized, so that a lone very
// bright pixel (a specular firefly) cannot flash a whole block of bloom while uniform areas keep their value. The
// levels after it hold filtered, finite values only.
#include "common/common.glsl"
#include "common/post.glsl"

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 oColor;

vec3 tap(vec2 uv) {
    vec3 color = textureLod(uSource, uv, 0.0).rgb;
#if defined(RV_BLOOM_KARIS)
    color = bloomExcess(sanitizeRadiance(color));
#endif
    return color;
}

#if defined(RV_BLOOM_KARIS)
// Box weight of the Karis average: `weight` / (1 + exposed luminance).
float karisWeight(vec3 box, float weight) {
    return weight / (1.0 + luminance(box) * uExposure);
}
#endif

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uSource, 0));
    // a . b . c
    // . j . k .
    // d . e . f
    // . l . m .
    // g . h . i      (one character per source texel; e at the center of the output texel)
    vec3 a = tap(vUv + texel * vec2(-2.0, 2.0));
    vec3 b = tap(vUv + texel * vec2(0.0, 2.0));
    vec3 c = tap(vUv + texel * vec2(2.0, 2.0));
    vec3 d = tap(vUv + texel * vec2(-2.0, 0.0));
    vec3 e = tap(vUv);
    vec3 f = tap(vUv + texel * vec2(2.0, 0.0));
    vec3 g = tap(vUv + texel * vec2(-2.0, -2.0));
    vec3 h = tap(vUv + texel * vec2(0.0, -2.0));
    vec3 i = tap(vUv + texel * vec2(2.0, -2.0));
    vec3 j = tap(vUv + texel * vec2(-1.0, 1.0));
    vec3 k = tap(vUv + texel * vec2(1.0, 1.0));
    vec3 l = tap(vUv + texel * vec2(-1.0, -1.0));
    vec3 m = tap(vUv + texel * vec2(1.0, -1.0));

    vec3 center = (j + k + l + m) * 0.25;
    vec3 topLeft = (a + b + d + e) * 0.25;
    vec3 topRight = (b + c + e + f) * 0.25;
    vec3 bottomLeft = (d + e + g + h) * 0.25;
    vec3 bottomRight = (e + f + h + i) * 0.25;
#if defined(RV_BLOOM_KARIS)
    float centerWeight = karisWeight(center, RV_BLOOM_CENTER_BOX_WEIGHT);
    float topLeftWeight = karisWeight(topLeft, RV_BLOOM_CORNER_BOX_WEIGHT);
    float topRightWeight = karisWeight(topRight, RV_BLOOM_CORNER_BOX_WEIGHT);
    float bottomLeftWeight = karisWeight(bottomLeft, RV_BLOOM_CORNER_BOX_WEIGHT);
    float bottomRightWeight = karisWeight(bottomRight, RV_BLOOM_CORNER_BOX_WEIGHT);
    vec3 sum = center * centerWeight + topLeft * topLeftWeight + topRight * topRightWeight +
               bottomLeft * bottomLeftWeight + bottomRight * bottomRightWeight;
    oColor = vec4(sum / (centerWeight + topLeftWeight + topRightWeight + bottomLeftWeight + bottomRightWeight), 1.0);
#else
    oColor = vec4(center * RV_BLOOM_CENTER_BOX_WEIGHT +
                      (topLeft + topRight + bottomLeft + bottomRight) * RV_BLOOM_CORNER_BOX_WEIGHT,
                  1.0);
#endif
}
