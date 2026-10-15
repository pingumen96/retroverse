// Declarations shared by every Retroverse shader: #include "common/common.glsl" right after #version.
//
// Uniform buffer binding points (single table: src/render/include/rv/render/uniform_blocks.hpp):
//   0 FrameUniforms (below), 1 lights, 2 material, 3+ reserved.
// Vertex attribute locations (rv::geometry::VertexLayout::standard(), rv::gl::GpuMesh):
//   0 position, 1 normal, 2 tangent (w = bitangent sign), 3 uv0, 4..7 per-instance model matrix columns.
// Conventions (ADR-0002): meters, +Y up, right-handed, counter-clockwise front faces, linear-space shading written to
// an sRGB framebuffer.

// Per-view constants. Mirrors rv::gl::FrameUniforms member for member (std140): change both together.
layout(std140, binding = 0) uniform FrameUniforms {
    mat4 uView;           // world -> view
    mat4 uProjection;     // view -> clip
    mat4 uViewProjection; // world -> clip
    vec3 uCameraPosition; // eye position, world space
    float uTime;          // simulated seconds since the start of the run
    float uExposure;      // scene exposure, applied before tonemapping
};
