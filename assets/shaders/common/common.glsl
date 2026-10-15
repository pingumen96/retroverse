// Declarations shared by every Retroverse shader: #include "common/common.glsl" right after #version.
//
// Uniform buffer binding points (single table: src/render/include/rv/render/uniform_blocks.hpp):
//   0 FrameUniforms (below), 1 LightsUniforms (common/lights.glsl), 2 MaterialUniforms (common/material.glsl),
//   3 PostUniforms (common/post.glsl), 4 ShadowUniforms (common/shadow.glsl), 5 SsaoUniforms (common/ssao.glsl),
//   6+ reserved.
// Texture units: 0..4 the material (common/material.glsl), 5..7 the post-processing (common/post.glsl,
// post/post.frag), 8 the shadow map (common/shadow.glsl), 9 the ambient occlusion (common/occlusion.glsl), 10..11 the
// ambient occlusion passes (common/ssao.glsl).
// Vertex attribute locations (rv::geometry::VertexLayout::standard(), rv::gl::GpuMesh):
//   0 position, 1 normal, 2 tangent (w = bitangent sign), 3 uv0, 4..7 per-instance model matrix columns.
// Conventions (ADR-0002): meters, +Y up, right-handed, counter-clockwise front faces, uv (0, 0) at the top-left of
// the image, linear-space shading. Scene shaders write linear radiance into an HDR target; the post pass
// (post/post.frag) tonemaps it into an sRGB target, which encodes its linear output (GL_FRAMEBUFFER_SRGB).

const float RV_PI = 3.14159265358979;

// Per-view constants. Mirrors rv::gl::FrameUniforms member for member (std140): change both together.
layout(std140, binding = 0) uniform FrameUniforms {
    mat4 uView;           // world -> view
    mat4 uProjection;     // view -> clip
    mat4 uViewProjection; // world -> clip
    vec3 uCameraPosition; // eye position, world space
    float uTime;          // simulated seconds since the start of the run
};

// v / |v|, or `fallback` when v is (nearly) zero: interpolated or perturbed vectors never turn into NaN.
vec3 safeNormalize(vec3 v, vec3 fallback) {
    float lengthSquared = dot(v, v);
    return lengthSquared > 1e-12 ? v * inversesqrt(lengthSquared) : fallback;
}
