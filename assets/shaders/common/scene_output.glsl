// Fragment outputs of the scene shaders (pbr/pbr.frag, unlit/unlit_emissive.frag) into the HDR scene target
// (rv::gl::ViewTargets::hdr()), at the locations of rv::gl::SceneOutput, which are also its color attachments
// (src/render/include/rv/render/uniform_blocks.hpp): the linear radiance a surface sends towards the eye, and the part
// of it the surface emits itself (screens, lamp shades, neon). The bloom always spreads the emitted light, of the
// reflected light only the highlights (common/post.glsl bloomSource), so that lit paper, wood and walls never glow.
// Neither output is exposed nor tonemapped: the post pass (post/post.frag) does that once for the whole view.

layout(location = 0) out vec4 oColor;    // radiance (reflected + emitted), with the material's output alpha
layout(location = 1) out vec4 oEmission; // the emitted part of oColor, with the same alpha (blended alike)

// Writes both outputs: `radiance` includes `emission`.
void writeSceneOutput(vec3 radiance, vec3 emission, float alpha) {
    oColor = vec4(radiance, alpha);
    oEmission = vec4(emission, alpha);
}
