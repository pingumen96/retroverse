#version 450 core
// Inspection views of the asset preview (rv::gl::SceneShaders::inspection): every mesh drawn in a false color that
// shows one property of its surface, unlit, with its own material bound. The view is shown without tonemapping
// (rv::gl::PostSettings::tonemap off: the HDR target is copied to the sRGB output, which encodes it), so the output is
// the linear form of the color to display. One program per view, selected by an injected define:
//   RV_INSPECT_BASE_COLOR  (the default) the base color, its factor times its texture and, for the collection's
//                          layered materials, the layer the instance names (linear: the output shows the authored
//                          sRGB);
//   RV_INSPECT_NORMALS     the shading normal in world space, normal maps included, shown as n * 0.5 + 0.5;
//   RV_INSPECT_UV          the first uv set: a checker of 8 x 8 squares per uv unit, red growing along u and green
//                          along v within each unit, so the layout's scale, orientation (uv (0, 0) is the image's
//                          top-left, ADR-0002), mirroring and seams show.
// Alpha-masked materials keep their cut-outs, blended ones their alpha. Nothing is emitted: views without tonemapping
// have no bloom. Vertex stage: pbr/pbr.vert.
#include "common/common.glsl"
#include "common/material.glsl"
#include "common/surface.glsl"
#include "common/srgb.glsl"
#include "common/scene_output.glsl"

// The uv checker of RV_INSPECT_UV as a display (sRGB-encoded) color.
vec3 uvChecker(vec2 uv) {
    vec2 square = floor(uv * 8.0);
    float light = mod(square.x + square.y, 2.0) < 0.5 ? 1.0 : 0.6;
    vec2 inUnit = fract(uv);
    return vec3(0.15 + 0.85 * inUnit.x, 0.15 + 0.85 * inUnit.y, 0.45) * light;
}

void main() {
    // Every texture is sampled before the alpha test may discard (common/material.glsl).
    vec4 baseColor = materialInstanceBaseColor(vUv, vInstance);
    vec3 N = shadingNormal();
    materialAlphaTest(baseColor.a);
#if defined(RV_INSPECT_NORMALS)
    vec3 color = srgbToLinear(N * 0.5 + 0.5);
#elif defined(RV_INSPECT_UV)
    vec3 color = srgbToLinear(uvChecker(vUv));
#else // RV_INSPECT_BASE_COLOR
    vec3 color = baseColor.rgb;
#endif
    writeSceneOutput(color, vec3(0.0), materialOutputAlpha(baseColor.a));
}
