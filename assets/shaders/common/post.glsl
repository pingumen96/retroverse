// Post-processing of a view (rv::gl::PostProcessor): the PostUniforms block at uniform binding 3, the source texture of
// the passes and the filters of the bloom chain. Mirrors rv::gl::PostUniforms and rv::gl::PostTextureUnit member for
// member (std140; src/render/include/rv/render/uniform_blocks.hpp), filled by rv::gl::makePostUniforms. Every pass
// draws the fullscreen triangle of post/fullscreen.vert; its sources are sampled bilinearly and clamped to their edges.
//
// Bloom (Jimenez 2014, "Next Generation Post Processing in Call of Duty: Advanced Warfare"): the light of the HDR scene
// above a soft threshold (bloomExcess) is downsampled into levels of half, a quarter, ... of the scene's size with the
// 13-tap filter, then each level is blended with the 3 x 3 tent upsampling of the next smaller one, from the smallest
// up. Both filters keep the energy of the image (their weights add up to 1), and so does the blend
// (rv::gl::bloomScatter); the post pass moves the share uBloomMix of that light into its blur.

layout(std140, binding = 3) uniform PostUniforms {
    float uExposure;        // > 0, multiplies the scene radiance (bloom included) before tonemapping
    float uBloomMix;        // [0, 1]: radiance + uBloomMix * (bloom - bloomExcess(radiance)); 0 leaves uBloom unread
    float uBloomThreshold;  // > 0: exposed brightness above which light blooms in full
    float uBloomKnee;       // [0, uBloomThreshold]: half width of the soft knee around the threshold
    float uVignette;        // [0, 1]: darkening of the corners
    float uDitherAmplitude; // [0, 1]: peak-to-peak noise added to the sRGB-encoded output (1/255: one 8-bit step)
};

// The image a pass filters: the HDR scene or a bloom level (PostTextureUnit::Source). The post pass also reads the
// bloom on unit 6 (post/post.frag); the bloom passes, which draw into the bloom levels, never declare it.
layout(binding = 5) uniform sampler2D uSource;

// Largest finite half float, the format of the HDR targets.
const float RV_MAX_RADIANCE = 65504.0;

// Weights of the 13-tap downsample: five overlapping 2 x 2 boxes of bilinear taps, the central box weighted 0.5 and the
// four corner boxes 0.125 each, together 1.
const float RV_BLOOM_CENTER_BOX_WEIGHT = 0.5;
const float RV_BLOOM_CORNER_BOX_WEIGHT = 0.125;

// Weights of the 3 x 3 tent filter of the upsampling (1 2 1 / 2 4 2 / 1 2 1, over 16), together 1.
const float RV_TENT_CENTER_WEIGHT = 0.25;
const float RV_TENT_EDGE_WEIGHT = 0.125;
const float RV_TENT_CORNER_WEIGHT = 0.0625;

// Radiance read from an HDR target, safe to filter and tonemap: NaN and negative components become 0, larger ones
// (an overflowed specular peak, stored as infinity) RV_MAX_RADIANCE, so a bad pixel never spreads through the bloom.
vec3 sanitizeRadiance(vec3 radiance) {
    radiance = mix(radiance, vec3(0.0), isnan(radiance));
    return clamp(radiance, vec3(0.0), vec3(RV_MAX_RADIANCE));
}

// Relative luminance of linear sRGB (Rec. 709 primaries).
float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

// The part of `radiance` that blooms: the light above uBloomThreshold of exposed brightness b (the largest exposed
// component), radiance * max(knee, b - threshold) / b, where the knee term (b - threshold + knee)^2 / (4 knee) for b
// within threshold +- knee starts the share smoothly at 0 below threshold - knee (Kino / Unity quadratic threshold).
// Lit walls and wood stay out of the bloom; screens, lamp shades, neon and highlights glow.
vec3 bloomExcess(vec3 radiance) {
    float brightness = max(radiance.r, max(radiance.g, radiance.b)) * uExposure;
    float knee = clamp(brightness - uBloomThreshold + uBloomKnee, 0.0, 2.0 * uBloomKnee);
    knee = knee * knee / max(4.0 * uBloomKnee, 1e-5);
    float excess = max(knee, brightness - uBloomThreshold);
    return radiance * (excess / max(brightness, 1e-5));
}

// 3 x 3 tent filter of `image` around `uv`, its taps one texel of `image` apart (nine bilinear fetches).
vec3 tentFilter(sampler2D image, vec2 uv) {
    vec2 texel = 1.0 / vec2(textureSize(image, 0));
    vec3 center = textureLod(image, uv, 0.0).rgb;
    vec3 edges = textureLod(image, uv + vec2(-texel.x, 0.0), 0.0).rgb;
    edges += textureLod(image, uv + vec2(texel.x, 0.0), 0.0).rgb;
    edges += textureLod(image, uv + vec2(0.0, -texel.y), 0.0).rgb;
    edges += textureLod(image, uv + vec2(0.0, texel.y), 0.0).rgb;
    vec3 corners = textureLod(image, uv + vec2(-texel.x, -texel.y), 0.0).rgb;
    corners += textureLod(image, uv + vec2(texel.x, -texel.y), 0.0).rgb;
    corners += textureLod(image, uv + vec2(-texel.x, texel.y), 0.0).rgb;
    corners += textureLod(image, uv + vec2(texel.x, texel.y), 0.0).rgb;
    return center * RV_TENT_CENTER_WEIGHT + edges * RV_TENT_EDGE_WEIGHT + corners * RV_TENT_CORNER_WEIGHT;
}
