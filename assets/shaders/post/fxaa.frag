#version 450 core
// Anti-aliasing (rv::gl::PostProcessor, common/post.glsl): fast approximate anti-aliasing of the tonemapped image
// (FXAA, after Lottes 2011, "FXAA", NVIDIA white paper), the last pass of a view. uSource is the image the post pass
// wrote into an sRGB target (post/post.frag): its colors read back linear, its alpha holds the perceptual luma of each
// pixel (the sRGB encoding of its luminance), which every edge test reads.
//
// A pixel whose luma differs little from its four neighbors (a range below RV_FXAA_RELATIVE_THRESHOLD of the brightest
// of them, or below RV_FXAA_ABSOLUTE_THRESHOLD) is copied as it is: flat areas, gradients and most texture detail.
// Otherwise the pass decides whether the edge through it runs horizontally or vertically, takes the neighbor across it
// with the steeper luma step, walks along the boundary between the two both ways until the luma there leaves the
// edge's average (the ends of the stair-step) and blends the pixel towards that neighbor by how close it lies to the
// nearer end: up to one half at an end, nothing in the middle, which turns the stair-steps of a slanted edge into a
// ramp. Features thinner than a pixel, whose steps are too short to walk, are blended by their contrast with the
// 3 x 3 average around them instead (RV_FXAA_SUBPIXEL). The blend is one bilinear tap between the pixel and its
// neighbor, in linear light. The output is linear; the sRGB target encodes it (GL_FRAMEBUFFER_SRGB).
#include "common/common.glsl"
#include "common/post.glsl"

layout(location = 0) in vec2 vUv;

layout(location = 0) out vec4 oColor;

// A pixel is on an edge when the luma range of its 4-neighborhood reaches this share of the brightest luma there...
const float RV_FXAA_RELATIVE_THRESHOLD = 0.125;
// ... and at least this much, so that the faint gradients and the noise of dark areas are left alone.
const float RV_FXAA_ABSOLUTE_THRESHOLD = 0.0625;
// Strength of the blending of sub-pixel features, in [0, 1]: the lower half keeps thin lines and small print sharper.
const float RV_FXAA_SUBPIXEL = 0.5;
// Taps of the walk along an edge, each way, and how far each one advances in pixels: single pixels first, then longer
// strides (each bilinear tap averages the two pixels across the boundary), 23.5 pixels each way in all.
const int RV_FXAA_SEARCH_STEPS = 10;
const float RV_FXAA_SEARCH_STRIDES[RV_FXAA_SEARCH_STEPS] = float[](1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 4.0, 8.0);
// An edge ends where the luma along it leaves the edge's average by this share of the luma step across it.
const float RV_FXAA_END_SHARE = 0.25;

// Perceptual luma at `uv`, interpolated between the pixels around it.
float lumaAt(vec2 uv) {
    return textureLod(uSource, uv, 0.0).a;
}

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uSource, 0));
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 uv = (vec2(pixel) + 0.5) * texel;
    vec4 center = texelFetch(uSource, pixel, 0);

    // The luma of the pixel and of its neighbors (north is +y; the sampler clamps them to the image at its borders).
    float lumaC = center.a;
    float lumaN = textureLodOffset(uSource, uv, 0.0, ivec2(0, 1)).a;
    float lumaS = textureLodOffset(uSource, uv, 0.0, ivec2(0, -1)).a;
    float lumaE = textureLodOffset(uSource, uv, 0.0, ivec2(1, 0)).a;
    float lumaW = textureLodOffset(uSource, uv, 0.0, ivec2(-1, 0)).a;
    float lumaMin = min(lumaC, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float lumaMax = max(lumaC, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float range = lumaMax - lumaMin;
    if (range < max(RV_FXAA_ABSOLUTE_THRESHOLD, lumaMax * RV_FXAA_RELATIVE_THRESHOLD)) {
        oColor = vec4(center.rgb, 1.0);
        return;
    }
    float lumaNE = textureLodOffset(uSource, uv, 0.0, ivec2(1, 1)).a;
    float lumaNW = textureLodOffset(uSource, uv, 0.0, ivec2(-1, 1)).a;
    float lumaSE = textureLodOffset(uSource, uv, 0.0, ivec2(1, -1)).a;
    float lumaSW = textureLodOffset(uSource, uv, 0.0, ivec2(-1, -1)).a;

    // Sub-pixel features: how far the pixel stands out from the low-pass average of its 3 x 3 neighborhood, relative
    // to the range, eased in so that only clear outliers blend much.
    float average = (2.0 * (lumaN + lumaS + lumaE + lumaW) + (lumaNE + lumaNW + lumaSE + lumaSW)) / 12.0;
    float subpixel = smoothstep(0.0, 1.0, clamp(abs(average - lumaC) / range, 0.0, 1.0));
    float subpixelBlend = subpixel * subpixel * RV_FXAA_SUBPIXEL;

    // Orientation: a horizontal edge changes the luma along y. The second differences across each direction, over the
    // three rows or columns of the neighborhood, the middle one weighted twice.
    float acrossY = abs(lumaNW - 2.0 * lumaW + lumaSW) + 2.0 * abs(lumaN - 2.0 * lumaC + lumaS) +
                    abs(lumaNE - 2.0 * lumaE + lumaSE);
    float acrossX = abs(lumaNW - 2.0 * lumaN + lumaNE) + 2.0 * abs(lumaW - 2.0 * lumaC + lumaE) +
                    abs(lumaSW - 2.0 * lumaS + lumaSE);
    bool horizontal = acrossY >= acrossX;

    // The neighbor across the edge: the side with the steeper luma step from the pixel.
    float lumaPositive = horizontal ? lumaN : lumaE;
    float lumaNegative = horizontal ? lumaS : lumaW;
    float stepPositive = abs(lumaPositive - lumaC);
    float stepNegative = abs(lumaNegative - lumaC);
    bool positive = stepPositive >= stepNegative;
    vec2 across = (horizontal ? vec2(0.0, texel.y) : vec2(texel.x, 0.0)) * (positive ? 1.0 : -1.0);
    vec2 along = horizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    float edgeLuma = 0.5 * (lumaC + (positive ? lumaPositive : lumaNegative));
    float endThreshold = RV_FXAA_END_SHARE * max(stepPositive, stepNegative);

    // The walk, on the boundary between the pixel and its neighbor across, both ways at once. endBack and endForward
    // hold the luma of the last tap of each way relative to the edge's average.
    vec2 uvBack = uv + 0.5 * across;
    vec2 uvForward = uvBack;
    float endBack = 0.0;
    float endForward = 0.0;
    bool backDone = false;
    bool forwardDone = false;
    for (int i = 0; i < RV_FXAA_SEARCH_STEPS && !(backDone && forwardDone); ++i) {
        if (!backDone) {
            uvBack -= along * RV_FXAA_SEARCH_STRIDES[i];
            endBack = lumaAt(uvBack) - edgeLuma;
            backDone = abs(endBack) >= endThreshold;
        }
        if (!forwardDone) {
            uvForward += along * RV_FXAA_SEARCH_STRIDES[i];
            endForward = lumaAt(uvForward) - edgeLuma;
            forwardDone = abs(endForward) >= endThreshold;
        }
    }
    float distanceBack = horizontal ? uv.x - uvBack.x : uv.y - uvBack.y;
    float distanceForward = horizontal ? uvForward.x - uv.x : uvForward.y - uv.y;
    bool backNearer = distanceBack < distanceForward;
    float nearest = min(distanceBack, distanceForward);
    float nearerEnd = backNearer ? endBack : endForward;
    // The pixel blends towards the color across only when the luma at the nearer end lies on the other side of the
    // edge's average than its own: there the edge steps into the pixel's row (or column), which the true, slanted edge
    // crosses partly. At an end where the edge steps away from it, the pixel lies wholly on its side and stays.
    bool pixelBelow = lumaC < edgeLuma;
    float edgeBlend = (nearerEnd < 0.0) != pixelBelow ? 0.5 - nearest / (distanceBack + distanceForward) : 0.0;
    float blend = max(edgeBlend, subpixelBlend);
    oColor = vec4(textureLod(uSource, uv + blend * across, 0.0).rgb, 1.0);
}
