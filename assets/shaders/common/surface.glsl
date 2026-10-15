// The inputs every mesh fragment stage receives from pbr/pbr.vert, and the surface normals built from them: the lit
// shader shades with them (pbr/pbr.frag), the inspection views show them (debug/inspect.frag). Include after
// common/material.glsl (the normal texture, its scale and the double-sided flag).

layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vNormal;  // world space, not normalized
layout(location = 2) in vec4 vTangent; // world-space xyz, bitangent sign in w
layout(location = 3) in vec2 vUv;

// Geometric normal: the interpolated vertex normal, reversed on the back faces of double-sided materials.
vec3 geometricNormal() {
    vec3 N = safeNormalize(vNormal, vec3(0.0, 1.0, 0.0));
    return materialHas(RV_MATERIAL_DOUBLE_SIDED) && !gl_FrontFacing ? -N : N;
}

// Shading normal: the interpolated vertex normal, perturbed by the normal texture in the tangent frame of ADR-0002
// (bitangent = cross(normal, tangent.xyz) * tangent.w), then reversed on the back faces of double-sided materials.
vec3 shadingNormal() {
    vec3 N = safeNormalize(vNormal, vec3(0.0, 1.0, 0.0));
    if (materialHas(RV_MATERIAL_NORMAL_TEXTURE)) {
        vec3 T = vTangent.xyz - N * dot(N, vTangent.xyz); // Gram-Schmidt against the interpolated normal
        if (dot(T, T) > 1e-12) {
            T = normalize(T);
            vec3 B = cross(N, T) * (vTangent.w < 0.0 ? -1.0 : 1.0);
            vec3 tangentNormal = texture(uNormalTexture, vUv).xyz * 2.0 - 1.0;
            tangentNormal.xy *= uNormalScale;
            N = safeNormalize(mat3(T, B, N) * tangentNormal, N);
        }
    }
    if (materialHas(RV_MATERIAL_DOUBLE_SIDED) && !gl_FrontFacing) {
        N = -N;
    }
    return N;
}
