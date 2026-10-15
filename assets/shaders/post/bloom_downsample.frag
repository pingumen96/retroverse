#version 450 core
// Bloom downsample (rv::gl::PostProcessor, common/post.glsl): the 13-tap filter of Jimenez 2014 from uSource (the HDR
// scene for the first level, else the previous level) into a level of half its size. Each output texel averages five
// overlapping 2 x 2 boxes of bilinear taps, one and two source texels around its center.
//
// RV_BLOOM_KARIS (injected define, the first level only): each tap reads the radiance of the HDR scene and its emitted
// part (uEmission), made safe to filter (sanitizeRadiance) and reduced to the light that blooms (bloomSource), and the
// taps are combined with the Karis average: each is weighted by its filter weight times karisWeight (1 / (1 + its
// exposed luminance)) and the weights are normalized. A lone very bright pixel (a specular firefly) then adds about as
// much as a tap of exposed luminance 1, whatever its value and wherever it falls under the pixel grid, so its glow
// stays steady as the camera moves; uniform areas keep their value, the edges of bright areas lose a little of their
// glow. The levels after it hold filtered, finite values only.
#include "common/common.glsl"
#include "common/post.glsl"

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 oColor;

vec3 tap(vec2 uv) {
    vec3 color = textureLod(uSource, uv, 0.0).rgb;
#if defined(RV_BLOOM_KARIS)
    color = bloomSource(sanitizeRadiance(color), sanitizeRadiance(textureLod(uEmission, uv, 0.0).rgb));
#endif
    return color;
}

#if defined(RV_BLOOM_KARIS)
// Adds `source` with its filter weight times its Karis weight to the running sums.
void addKarisTap(vec3 source, float filterWeight, inout vec3 sum, inout float weightSum) {
    float weight = filterWeight * karisWeight(source);
    sum += source * weight;
    weightSum += weight;
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

#if defined(RV_BLOOM_KARIS)
    // The filter weight of a tap is a quarter of the weight of each box it belongs to: j, k, l, m the central box only,
    // e all four corner boxes, b, d, f, h two of them, a, c, g, i one; together 1, as in the plain filter below.
    const float centerBoxTap = RV_BLOOM_CENTER_BOX_WEIGHT * 0.25;
    const float cornerBoxTap = RV_BLOOM_CORNER_BOX_WEIGHT * 0.25;
    vec3 sum = vec3(0.0);
    float weightSum = 0.0;
    addKarisTap(j, centerBoxTap, sum, weightSum);
    addKarisTap(k, centerBoxTap, sum, weightSum);
    addKarisTap(l, centerBoxTap, sum, weightSum);
    addKarisTap(m, centerBoxTap, sum, weightSum);
    addKarisTap(e, 4.0 * cornerBoxTap, sum, weightSum);
    addKarisTap(b, 2.0 * cornerBoxTap, sum, weightSum);
    addKarisTap(d, 2.0 * cornerBoxTap, sum, weightSum);
    addKarisTap(f, 2.0 * cornerBoxTap, sum, weightSum);
    addKarisTap(h, 2.0 * cornerBoxTap, sum, weightSum);
    addKarisTap(a, cornerBoxTap, sum, weightSum);
    addKarisTap(c, cornerBoxTap, sum, weightSum);
    addKarisTap(g, cornerBoxTap, sum, weightSum);
    addKarisTap(i, cornerBoxTap, sum, weightSum);
    oColor = vec4(sum / max(weightSum, 1e-30), 1.0); // every weight is > 0 unless the exposure is absurdly high
#else
    vec3 center = (j + k + l + m) * 0.25;
    vec3 topLeft = (a + b + d + e) * 0.25;
    vec3 topRight = (b + c + e + f) * 0.25;
    vec3 bottomLeft = (d + e + g + h) * 0.25;
    vec3 bottomRight = (e + f + h + i) * 0.25;
    oColor = vec4(center * RV_BLOOM_CENTER_BOX_WEIGHT +
                      (topLeft + topRight + bottomLeft + bottomRight) * RV_BLOOM_CORNER_BOX_WEIGHT,
                  1.0);
#endif
}
