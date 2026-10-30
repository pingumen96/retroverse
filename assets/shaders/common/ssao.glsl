// Screen-space ambient occlusion (rv::gl::SsaoPass): the SsaoUniforms block at uniform binding 5 and the textures of
// the two passes. Mirrors rv::gl::SsaoUniforms, rv::gl::kSsaoSampleCount and rv::gl::SsaoTextureUnit member for member
// (std140; src/render/include/rv/render/uniform_blocks.hpp), filled by rv::gl::makeSsaoUniforms. Both passes draw the
// fullscreen triangle of post/fullscreen.vert and read the view's camera from FrameUniforms (common/common.glsl, whose
// projection is glm's perspectiveRH_NO on the desktop, or the off-axis projection of a headset's eye: depth in [-1, 1]
// either way, the eye's field shifted by uProjection[2][0] and [2][1], 0 for the desktop's symmetric field).
//
//  1. ssao/ssao.frag, at half the resolution of the view: per pixel, the view-space position of the full-resolution
//     pixel at twice its coordinates and its normal (both from the scene's depth), then kSsaoSampleCount points of the
//     hemisphere above the surface (the kernel scaled by the radius and rotated about the normal by a 4 x 4 pattern of
//     angles): a sample counts as occluded where the scene's surface at its pixel is in front of it by more than the
//     bias, weighted down as that surface's depth nears the radius away from the point's, and not at all beyond it (a
//     foreground object farther than the radius from the background does not darken it: no halos). Output: the
//     unoccluded share raised to the intensity, and the point's distance for the blur.
//  2. ssao/ssao_blur.frag, at full resolution: the 4 x 4 half-resolution estimates around each pixel (one of each
//     rotation of the pattern, so the noise averages out), each weighted by how close its depth is to the depth the
//     pixel's surface has at its place (the plane of the pixel, from the gradient of the reciprocal depth, which is
//     linear in screen space on a plane): flat surfaces blur fully, even at grazing angles, and the occlusion of one
//     surface never bleeds onto another in front of or behind it (no halos).

#define RV_SSAO_SAMPLE_COUNT 16

layout(std140, binding = 5) uniform SsaoUniforms {
    vec4 uSsaoKernel[RV_SSAO_SAMPLE_COUNT]; // xyz: offsets in the unit hemisphere around +Z
    float uSsaoRadius;         // meters
    float uSsaoIntensity;      // exponent of the unoccluded share
    float uSsaoBias;           // meters an occluder must be in front of a sample
    float uSsaoDepthTolerance; // blur: relative depth difference where a tap stops counting
};

// Window depth of the view's HDR scene target, in [0, 1]: read exactly with texelFetch, or between pixels with the
// bilinear sampler bound to the unit (on a plane, window depth is linear in screen space).
layout(binding = 10) uniform sampler2D uSceneDepth;
// The half-resolution estimate of pass 1 (r: unoccluded share, g: distance of its point), read by the blur.
layout(binding = 11) uniform sampler2D uRawOcclusion;

// View-space z of window depth `depth` (negative in front of the camera; the far plane at depth 1).
float viewZ(float depth) {
    return -uProjection[3][2] / (depth * 2.0 - 1.0 + uProjection[2][2]);
}

// Reciprocal distance along the view axis (1 / -z) of window depth `depth`: like window depth, linear in screen space
// on a plane.
float inverseDistance(float depth) {
    return -1.0 / viewZ(depth);
}

// View-space position of the surface at the center of full-resolution `pixel` with window depth `depth`: the inverse of
// ndc.x = (m00 x + m20 z) / -z (and likewise for y), m20 and m21 being 0 for a symmetric field.
vec3 viewPosition(vec2 pixel, float depth) {
    vec2 ndc = (pixel + 0.5) / vec2(textureSize(uSceneDepth, 0)) * 2.0 - 1.0;
    float z = viewZ(depth);
    vec2 shifted = ndc + vec2(uProjection[2][0], uProjection[2][1]);
    return vec3(shifted.x * -z / uProjection[0][0], shifted.y * -z / uProjection[1][1], z);
}

// Window depth at full-resolution `pixel`, clamped to the target.
float sceneDepthAt(ivec2 pixel) {
    ivec2 last = textureSize(uSceneDepth, 0) - 1;
    return texelFetch(uSceneDepth, clamp(pixel, ivec2(0), last), 0).r;
}

// Whether full-resolution `pixel` lies inside the target. Differences of the depth along the image use only neighbors
// inside it: clamped to the border, a neighbor would repeat the border pixel's depth, a step with no change in depth
// that looks like a surface facing the camera whatever its orientation.
bool insideScene(ivec2 pixel) {
    return all(greaterThanEqual(pixel, ivec2(0))) && all(lessThan(pixel, textureSize(uSceneDepth, 0)));
}
