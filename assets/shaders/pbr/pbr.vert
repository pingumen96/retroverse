#version 450 core
// Mesh vertex stage shared by the lit program (pbr/pbr.frag), the unlit one (unlit/unlit_emissive.frag) and the
// depth-only ones (depth/depth.frag: the shadow map and the depth pre-pass): world position, normal and tangent for
// shading, uv0 for textures. Every draw is instanced: each instance reads its record from the instance buffer
// (rv::scene::InstanceData through rv::gl::GpuMesh, divisor 1): the model matrix at locations 4..7, its attributes at
// location 8, handed on unchanged to the fragment stages that read them (vInstance); an instance flagged
// RV_INSTANCE_HIDDEN is not drawn (a static instance group keeps its record). gl_Position is invariant: every
// program computes the same depth for a vertex, so the lit pass passes the depth test (GL_LEQUAL) exactly where the
// depth pre-pass left its surfaces.
#include "common/common.glsl"

invariant gl_Position;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;   // xyz tangent, w = bitangent sign (ADR-0002)
layout(location = 3) in vec2 aUv0;
layout(location = 4) in mat4 aModel;     // per instance, local -> world: columns at locations 4..7
layout(location = 8) in uvec4 aInstance; // per instance: front layer, spine layer, flags (RV_INSTANCE_*), pick id

layout(location = 0) out vec3 vWorldPosition;
layout(location = 1) out vec3 vNormal;  // world space, not normalized
layout(location = 2) out vec4 vTangent; // world-space xyz, bitangent sign in w
layout(location = 3) out vec2 vUv;
layout(location = 4) flat out uvec4 vInstance; // aInstance, the same for every fragment of the instance

void main() {
    vec4 world = aModel * vec4(aPosition, 1.0);
    // The inverse transpose keeps normals perpendicular to the surface under non-uniform scales; tangents follow the
    // surface, so they use the model matrix itself.
    mat3 normalMatrix = transpose(inverse(mat3(aModel)));
    vWorldPosition = world.xyz;
    vNormal = normalMatrix * aNormal;
    vTangent = vec4(mat3(aModel) * aTangent.xyz, aTangent.w);
    vUv = aUv0;
    vInstance = aInstance;
    gl_Position = uViewProjection * world;
    if ((aInstance.z & RV_INSTANCE_HIDDEN) != 0u) {
        // Pulled out or inspected, drawn by the dynamic path instead: every vertex beyond the clip volume's right edge,
        // so the instance draws nothing in any pass (the shadow map and the depth pre-pass included).
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    }
}
