#version 450 core
// PBR_Base (style §25): metallic-roughness materials lit by the scene's point, spot and directional lights
// (common/lights.glsl), the key light shadowed by its shadow map (common/shadow.glsl), and a hemispherical sky/ground
// ambient darkened by the view's ambient occlusion (common/occlusion.glsl), with optional base color,
// metallic-roughness, normal, occlusion and emissive textures (the surface normals: common/surface.glsl) and, for the
// collection's layered materials, the layer of a texture array the instance names (common/material.glsl). Output
// (common/scene_output.glsl): the linear scene radiance and its emissive part into the HDR target; exposure, bloom and
// tonemapping belong to the post pass (post/post.frag).
#include "common/common.glsl"
#include "common/lights.glsl"
#include "common/brdf.glsl"
#include "common/material.glsl"
#include "common/surface.glsl"
#include "common/shadow.glsl"
#include "common/occlusion.glsl"
#include "common/scene_output.glsl"

void main() {
    // Every texture is sampled before the alpha test: after a discard taken by only some fragments of a 2x2 quad,
    // implicit derivatives, hence the mip level and anisotropy of texture(), are undefined (GLSL 4.50 §6.4).
    vec4 baseColor = materialInstanceBaseColor(vUv, vInstance);
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
    // Shadow lookups move along the geometric normal: a normal map does not move the surface.
    vec3 shadowNormal = geometricNormal();

    vec3 radiance = vec3(0.0);
    int lightCount = min(uPunctualLightCount, RV_MAX_PUNCTUAL_LIGHTS);
    for (int i = 0; i < lightCount; ++i) {
        vec3 L;
        vec3 incoming = punctualLightRadiance(i, vWorldPosition, L);
        if (incoming == vec3(0.0)) {
            continue; // beyond the range or outside the cone
        }
        if (i == uShadowLightIndex) {
            incoming *= keyLightVisibility(vWorldPosition, shadowNormal);
        }
        radiance += evaluateLight(surface, N, V, L, incoming);
    }
    if (uDirectionalIntensity > 0.0) {
        float visibility =
            uShadowLightIndex == RV_DIRECTIONAL_SHADOW_LIGHT ? keyLightVisibility(vWorldPosition, shadowNormal) : 1.0;
        radiance += evaluateLight(surface, N, V, -uDirectionalDirection,
                                  uDirectionalColor * (uDirectionalIntensity * visibility));
    }

    // Hemispherical ambient, darkened by the material's occlusion and the view's ambient occlusion (not where blended
    // surfaces such as glass lie in front of the surfaces it was estimated on): diffuse from the hemisphere around the
    // normal, mix(ground, sky, n.y * 0.5 + 0.5) * albedo * ambientIntensity, and its specular counterpart seen along
    // the reflection (bent towards the normal as the lobe widens), weighted by the environment BRDF so that metals
    // reflect the room tinted by their base color, and occluded like it (specular occlusion).
    float NoV = max(dot(N, V), 1e-4);
    float ambientVisibility = materialHas(RV_MATERIAL_ALPHA_BLEND) ? 1.0 : screenAmbientOcclusion();
    vec3 reflected = reflect(-V, N);
    vec3 specularDirection = safeNormalize(mix(reflected, N, surface.alpha), N);
    radiance += hemisphereAmbient(N) * surface.diffuseColor * (occlusion * ambientVisibility);
    radiance += hemisphereAmbient(specularDirection) * environmentBrdf(surface.f0, surface.perceptualRoughness, NoV) *
                (occlusion * specularOcclusion(NoV, ambientVisibility, surface.alpha));
    radiance += emissive;

    writeSceneOutput(radiance, emissive, materialOutputAlpha(baseColor.a));
}
