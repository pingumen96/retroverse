#version 450 core
// Post pass (rv::gl::PostProcessor, common/post.glsl): the HDR scene (uSource and its emitted part uEmission, one
// texel per fragment: the output target has its size) with the share uBloomMix of its light that blooms (bloomSource)
// replaced by the bloom (the tent upsampling of the largest bloom level), the vignette, then exposure and the ACES
// fitted curve (common/tonemap.glsl), dithered against banding. The output is linear; the sRGB target encodes it
// (GL_FRAMEBUFFER_SRGB). The sRGB self-check takes a grey through this very pass (rv::app::runSrgbSelfCheck).
#include "common/common.glsl"
#include "common/post.glsl"
#include "common/tonemap.glsl"

// The bloom of the view: the largest bloom level (PostTextureUnit::Bloom), half the size of the scene.
layout(binding = 6) uniform sampler2D uBloom;

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 oColor;

// sRGB transfer functions (IEC 61966-2-1), as the target applies them.
vec3 linearToSrgb(vec3 linear) {
    linear = clamp(linear, 0.0, 1.0);
    vec3 curve = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
    return mix(curve, linear * 12.92, lessThanEqual(linear, vec3(0.0031308)));
}

vec3 srgbToLinear(vec3 encoded) {
    vec3 curve = pow((encoded + 0.055) / 1.055, vec3(2.4));
    return mix(curve, encoded / 12.92, lessThanEqual(encoded, vec3(0.04045)));
}

// Interleaved gradient noise (Jimenez 2014): a threshold in [0, 1) per pixel that changes from pixel to pixel with no
// visible pattern; fixed in time, so screenshots are reproducible.
float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

// Darkening towards the corners: 1 - uVignette * r^4, with r = 0 at the center of the view and 1 in its corners.
float vignette(vec2 uv) {
    vec2 fromCenter = uv * 2.0 - 1.0;
    float r2 = 0.5 * dot(fromCenter, fromCenter);
    return 1.0 - uVignette * r2 * r2;
}

// `display` with uDitherAmplitude of noise added where the target quantizes: in its sRGB encoding.
vec3 dither(vec3 display, vec2 pixel) {
    if (uDitherAmplitude <= 0.0) {
        return display;
    }
    float noise = interleavedGradientNoise(pixel) - 0.5;
    return srgbToLinear(clamp(linearToSrgb(display) + noise * uDitherAmplitude, 0.0, 1.0));
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 radiance = sanitizeRadiance(texelFetch(uSource, pixel, 0).rgb);
    if (uBloomMix > 0.0) {
        // The bloom holds the blurred light that blooms, so this moves light without adding any.
        vec3 emission = sanitizeRadiance(texelFetch(uEmission, pixel, 0).rgb);
        vec3 bloom = sanitizeRadiance(tentFilter(uBloom, vUv));
        radiance += uBloomMix * (bloom - bloomSource(radiance, emission));
    }
    radiance *= vignette(vUv);
    vec3 display = tonemap(radiance, uExposure);
    oColor = vec4(dither(display, gl_FragCoord.xy), 1.0);
}
