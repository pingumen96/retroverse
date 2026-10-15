#version 450 core
// Ambient occlusion estimate (rv::gl::SsaoPass, pass 1 of common/ssao.glsl) at half the resolution of the view, into
// rv::gl::ViewTargets::ambientOcclusionRaw(): r the unoccluded share of the ambient light raised to the intensity, g
// the distance in meters of the point it was estimated at (for the depth-aware blur, ssao/ssao_blur.frag).
#include "common/common.glsl"
#include "common/ssao.glsl"

layout(location = 0) out vec4 oOcclusion;

// Per pixel, the kernel is rotated about the normal and scaled, both from the pixel's place in a 4 x 4 Bayer pattern:
// neighboring pixels sample far-apart directions and distances, and the 4 x 4 window of the blur sees every rotation
// and scale once, so the occlusion of each surface is averaged over 256 sample points. The scaling spreads the
// silhouettes that samples near the normal would otherwise trace sharply behind an occluder.
const float RV_SSAO_PATTERN[16] =
    float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
// Smallest kernel scale of the pattern; the largest is 1.
const float RV_SSAO_MIN_SCALE = 0.5;

// Largest relative error of the plane test of viewTangent: a 1 cm step 20 m away still fails it, a plane at any angle
// passes it whatever the depth precision.
const float RV_SSAO_PLANE_TOLERANCE = 0.0005;
// Plane error of a side of viewTangent whose neighbors are not all inside the target: that side is never chosen.
const float RV_SSAO_OUTSIDE = 1e30;
// Share of the radius up to which an occluder's depth distance from the point counts fully (fading out to the radius).
const float RV_SSAO_FULL_RANGE = 0.75;

// Plane error of the side of full-resolution `pixel` towards `step`: how far the next two neighbors that way, `near`
// (window depth, at pixel + step) and the one after it, extrapolate the center's reciprocal distance `own`.
// RV_SSAO_OUTSIDE when the second neighbor lies outside the target (at the image's borders).
float planeError(ivec2 pixel, ivec2 step, float near, float own) {
    float error = abs(2.0 * inverseDistance(near) - inverseDistance(sceneDepthAt(pixel + 2 * step)) - own);
    return insideScene(pixel + 2 * step) ? error : RV_SSAO_OUTSIDE;
}

// Tangent of the surface at full-resolution `pixel` (whose view-space position is `center`, window depth `depth`) along
// `step`: towards the side whose neighbors continue the center's plane. The reciprocal distance is linear in screen
// space on a plane, so the side where the two next neighbors extrapolate the center's best is on its surface; this
// keeps normals right along creases (a wall meeting the floor) and the edges of objects, and at the image's borders,
// where only the side inside the target is used. `reliable` becomes false when neither side continues the plane (a
// sliver one pixel thin, such as the top of a baseboard seen from afar): its normal is unknown.
vec3 viewTangent(ivec2 pixel, ivec2 step, vec3 center, float depth, inout bool reliable) {
    float before1 = sceneDepthAt(pixel - step);
    float after1 = sceneDepthAt(pixel + step);
    float own = inverseDistance(depth);
    float errorBefore = planeError(pixel, -step, before1, own);
    float errorAfter = planeError(pixel, step, after1, own);
    reliable = reliable && min(errorBefore, errorAfter) <= RV_SSAO_PLANE_TOLERANCE * own;
    if (errorAfter < errorBefore) {
        return viewPosition(vec2(pixel + step), after1) - center;
    }
    return center - viewPosition(vec2(pixel - step), before1);
}

void main() {
    ivec2 cell = ivec2(gl_FragCoord.xy);
    ivec2 pixel = min(cell * 2, textureSize(uSceneDepth, 0) - 1);
    float depth = texelFetch(uSceneDepth, pixel, 0).r;
    if (depth >= 1.0) {
        oOcclusion = vec4(1.0, -viewZ(1.0), 0.0, 1.0); // the background: nothing to occlude
        return;
    }
    vec3 position = viewPosition(vec2(pixel), depth);
    // The normal faces the camera: the cross product of the tangents along +x and +y (right-handed view space).
    bool reliable = true;
    vec3 alongX = viewTangent(pixel, ivec2(1, 0), position, depth, reliable);
    vec3 alongY = viewTangent(pixel, ivec2(0, 1), position, depth, reliable);
    if (!reliable) {
        oOcclusion = vec4(1.0, 0.0, 0.0, 1.0); // no estimate: a distance of 0 makes the blur skip it
        return;
    }
    vec3 normal = safeNormalize(cross(alongX, alongY), vec3(0.0, 0.0, 1.0));
    ivec2 patternCell = cell & 3;
    float pattern = RV_SSAO_PATTERN[patternCell.y * 4 + patternCell.x];
    float angle = (pattern + 0.5) * (2.0 * RV_PI / 16.0);
    // A scale that does not follow the rotation (7 is coprime with 16: a permutation of the sixteen steps).
    float scale = mix(RV_SSAO_MIN_SCALE, 1.0, (mod(pattern * 7.0, 16.0) + 0.5) / 16.0);
    vec3 rotation = vec3(cos(angle), sin(angle), 0.0);
    // Tangent frame around the normal; a normal in the plane of the pattern takes any other tangent.
    vec3 tangent = safeNormalize(rotation - normal * dot(rotation, normal),
                                 safeNormalize(cross(normal, vec3(0.0, 0.0, 1.0)), vec3(1.0, 0.0, 0.0)));
    mat3 basis = mat3(tangent, cross(normal, tangent), normal);

    float occlusion = 0.0;
    for (int i = 0; i < RV_SSAO_SAMPLE_COUNT; ++i) {
        vec3 samplePosition = position + basis * (uSsaoKernel[i].xyz * (uSsaoRadius * scale));
        vec4 clip = uProjection * vec4(samplePosition, 1.0);
        if (clip.w <= 0.0) {
            continue; // behind the camera
        }
        vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
            continue; // off screen: nothing is known there, the sample counts as unoccluded
        }
        float surfaceZ = viewZ(textureLod(uSceneDepth, uv, 0.0).r);
        // Only occluders within the radius of the point count: an object farther in front of a wall than the radius (a
        // lamp pole, the face of a TV) leaves no halo on it. The depth buffer holds front surfaces only, so the weight
        // fades with their depth distance from the point, fully counted up to RV_SSAO_FULL_RANGE of the radius.
        float range = 1.0 - smoothstep(RV_SSAO_FULL_RANGE * uSsaoRadius, uSsaoRadius, abs(position.z - surfaceZ));
        occlusion += (surfaceZ >= samplePosition.z + uSsaoBias ? 1.0 : 0.0) * range;
    }
    float visibility = 1.0 - occlusion / float(RV_SSAO_SAMPLE_COUNT);
    oOcclusion = vec4(pow(max(visibility, 1e-4), uSsaoIntensity), -position.z, 0.0, 1.0);
}
