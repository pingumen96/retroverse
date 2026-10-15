#version 450 core
// Depth-aware blur of the ambient occlusion (rv::gl::SsaoPass, pass 2 of common/ssao.glsl) from the half-resolution
// estimate (ssao/ssao.frag) to the full resolution of the view, into rv::gl::ViewTargets::ambientOcclusion(), which the
// lit shader reads (common/occlusion.glsl). The background stays unoccluded.
#include "common/common.glsl"
#include "common/ssao.glsl"

layout(location = 0) out vec4 oOcclusion;

// Reciprocal distance (1 / -z) of the surface at full-resolution `pixel`: linear in screen space on a plane.
float inverseDistanceAt(ivec2 pixel) {
    return inverseDistance(sceneDepthAt(pixel));
}

// Change of the reciprocal distance per pixel along `step` at `pixel`, whose own is `center`: towards the neighbor that
// continues its surface best, so that the plane of a pixel at the edge of an object is its own; at the image's borders,
// towards the neighbor inside the target (0 for a target one pixel wide).
float inverseDistanceSlope(ivec2 pixel, ivec2 step, float center) {
    float forward = inverseDistanceAt(pixel + step) - center;
    float backward = center - inverseDistanceAt(pixel - step);
    if (!insideScene(pixel - step)) {
        return forward;
    }
    if (!insideScene(pixel + step)) {
        return backward;
    }
    return abs(forward) < abs(backward) ? forward : backward;
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(uSceneDepth, pixel, 0).r;
    if (depth >= 1.0) {
        oOcclusion = vec4(1.0);
        return;
    }
    float center = inverseDistance(depth);
    vec2 slope = vec2(inverseDistanceSlope(pixel, ivec2(1, 0), center),
                      inverseDistanceSlope(pixel, ivec2(0, 1), center));
    // The 4 x 4 estimates around the pixel: two half-resolution texels before it to one after for even coordinates, one
    // before to two after for odd ones, so that the window is centered on the pixel within half a pixel. At the image's
    // borders the window moves inside the target instead of repeating its edge, so it still holds one estimate of each
    // rotation of the pattern (clamping each tap remains for targets narrower than the window).
    ivec2 last = textureSize(uRawOcclusion, 0) - 1;
    ivec2 first = clamp((pixel >> 1) - 2 + (pixel & 1), ivec2(0), max(last - 3, ivec2(0)));
    float sum = 0.0;
    float weightSum = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            ivec2 tap = clamp(first + ivec2(x, y), ivec2(0), last);
            vec2 estimate = texelFetch(uRawOcclusion, tap, 0).rg;
            // Each estimate was taken at full-resolution pixel tap * 2: the pixel's plane predicts its distance there
            // (a distance of 0, an estimate without a usable normal, is never close to it).
            float expected = center + dot(slope, vec2(tap * 2 - pixel));
            float difference = abs(1.0 / max(estimate.g, 1e-4) - expected) / max(abs(expected), 1e-6);
            float weight = max(0.0, 1.0 - difference / uSsaoDepthTolerance);
            sum += estimate.r * weight;
            weightSum += weight;
        }
    }
    // No estimate of this surface nearby (a sliver thinner than the pattern): unoccluded, rather than another
    // surface's.
    oOcclusion = vec4(weightSum > 1e-3 ? sum / weightSum : 1.0, 0.0, 0.0, 1.0);
}
