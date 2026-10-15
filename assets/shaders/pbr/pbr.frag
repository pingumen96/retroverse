#version 450 core
// PBR_Base (style §25): metallic-roughness materials lit by the scene's point, spot and directional lights
// (common/lights.glsl) and a hemispherical sky/ground ambient, with optional base color, metallic-roughness, normal,
// occlusion and emissive textures. Output: exposure and the ACES fitted curve (common/tonemap.glsl), linear, encoded
// by the sRGB framebuffer.
#include "common/common.glsl"
#include "common/lights.glsl"
#include "common/brdf.glsl"
#include "common/tonemap.glsl"
#include "common/material.glsl"

layout(location = 0) in vec3 vWorldPosition;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv;

layout(location = 0) out vec4 oColor;

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

void main() {
    // Every texture is sampled before the alpha test: after a discard taken by only some fragments of a 2x2 quad,
    // implicit derivatives, hence the mip level and anisotropy of texture(), are undefined (GLSL 4.50 §6.4).
    vec4 baseColor = materialBaseColor(vUv);
    float metallic = uMetallic;
    float roughness = uRoughness;
    if (materialHas(RV_MATERIAL_METALLIC_ROUGHNESS_TEXTURE)) {
        vec4 metallicRoughness = texture(uMetallicRoughnessTexture, vUv);
        roughness *= metallicRoughness.g;
        metallic *= metallicRoughness.b;
    }
    float occlusion = 1.0;
    if (materialHas(RV_MATERIAL_OCCLUSION_TEXTURE)) {
        occlusion = mix(1.0, texture(uOcclusionTexture, vUv).r, uOcclusionStrength);
    }
    vec3 emissive = uEmissive;
    if (materialHas(RV_MATERIAL_EMISSIVE_TEXTURE)) {
        emissive *= texture(uEmissiveTexture, vUv).rgb;
    }

    vec3 N = shadingNormal();
    materialAlphaTest(baseColor.a);

    vec3 V = safeNormalize(uCameraPosition - vWorldPosition, N);
    BrdfSurface surface = makeBrdfSurface(baseColor.rgb, metallic, roughness);

    vec3 radiance = vec3(0.0);
    int lightCount = min(uPunctualLightCount, RV_MAX_PUNCTUAL_LIGHTS);
    for (int i = 0; i < lightCount; ++i) {
        PunctualLight light = uPunctualLights[i];
        vec3 toLight = light.position - vWorldPosition;
        float distanceSquared = dot(toLight, toLight);
        if (distanceSquared >= light.range * light.range) {
            continue; // the window is exactly 0 from the range on
        }
        vec3 L = toLight * inversesqrt(max(distanceSquared, 1e-8));
        float attenuation = distanceAttenuation(distanceSquared, light.range) * spotFactor(light, L);
        radiance += evaluateLight(surface, N, V, L, light.color * (light.intensity * attenuation));
    }
    if (uDirectionalIntensity > 0.0) {
        radiance += evaluateLight(surface, N, V, -uDirectionalDirection, uDirectionalColor * uDirectionalIntensity);
    }

    // Hemispherical ambient, darkened by occlusion: diffuse from the hemisphere around the normal,
    // mix(ground, sky, n.y * 0.5 + 0.5) * albedo * ambientIntensity, and its specular counterpart seen along the
    // reflection (bent towards the normal as the lobe widens), weighted by the environment BRDF so that metals
    // reflect the room tinted by their base color.
    float NoV = max(dot(N, V), 1e-4);
    vec3 reflected = reflect(-V, N);
    vec3 specularDirection = safeNormalize(mix(reflected, N, surface.alpha), N);
    radiance += hemisphereAmbient(N) * surface.diffuseColor * occlusion;
    radiance += hemisphereAmbient(specularDirection) *
                environmentBrdf(surface.f0, surface.perceptualRoughness, NoV) * occlusion;
    radiance += emissive;

    oColor = vec4(tonemap(radiance, uExposure), materialOutputAlpha(baseColor.a));
}
