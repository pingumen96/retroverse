// Metallic-roughness BRDF (glTF 2.0 model, style §25 PBR_Base): GGX normal distribution, height-correlated Smith
// visibility, Schlick Fresnel and a Lambert diffuse lobe. Needs common/common.glsl.
//
// Roughness: `perceptualRoughness` is the material value in [0, 1]; the lobes use alpha = perceptualRoughness^2.

// Smallest perceptual roughness used for shading: mirror-like values alias and their GGX peak overflows halves.
const float RV_MIN_PERCEPTUAL_ROUGHNESS = 0.045;
// Reflectance at normal incidence of dielectrics (4%, glTF).
const vec3 RV_DIELECTRIC_F0 = vec3(0.04);

// Surface terms shared by every light.
struct BrdfSurface {
    vec3 diffuseColor;         // baseColor * (1 - metallic): metals have no diffuse lobe
    vec3 f0;                   // mix(0.04, baseColor, metallic): metals reflect tinted by their base color
    float perceptualRoughness; // clamped to [RV_MIN_PERCEPTUAL_ROUGHNESS, 1]
    float alpha;               // perceptualRoughness^2
};

BrdfSurface makeBrdfSurface(vec3 baseColor, float metallic, float perceptualRoughness) {
    BrdfSurface surface;
    surface.diffuseColor = baseColor * (1.0 - metallic);
    surface.f0 = mix(RV_DIELECTRIC_F0, baseColor, metallic);
    surface.perceptualRoughness = clamp(perceptualRoughness, RV_MIN_PERCEPTUAL_ROUGHNESS, 1.0);
    surface.alpha = surface.perceptualRoughness * surface.perceptualRoughness;
    return surface;
}

// GGX (Trowbridge-Reitz) normal distribution function.
float distributionGgx(float NoH, float alpha) {
    float alphaSquared = alpha * alpha;
    float f = (NoH * alphaSquared - NoH) * NoH + 1.0;
    return alphaSquared / (RV_PI * f * f);
}

// Height-correlated Smith visibility V = G / (4 NoL NoV) (Heitz 2014).
float visibilitySmithGgxCorrelated(float NoV, float NoL, float alpha) {
    float alphaSquared = alpha * alpha;
    float lambdaV = NoL * sqrt(NoV * NoV * (1.0 - alphaSquared) + alphaSquared);
    float lambdaL = NoV * sqrt(NoL * NoL * (1.0 - alphaSquared) + alphaSquared);
    return 0.5 / max(lambdaV + lambdaL, 1e-5);
}

// Schlick's Fresnel approximation.
vec3 fresnelSchlick(vec3 f0, float VoH) {
    float f = 1.0 - VoH;
    float f2 = f * f;
    return f0 + (vec3(1.0) - f0) * (f2 * f2 * f);
}

// Lambertian diffuse lobe.
float diffuseLambert() {
    return 1.0 / RV_PI;
}

// Radiance reflected towards V (unit, surface -> eye) by one light arriving along L (unit, surface -> light) with
// `radiance` (color * intensity * attenuation), for the unit normal N.
vec3 evaluateLight(BrdfSurface surface, vec3 N, vec3 V, vec3 L, vec3 radiance) {
    float NoL = dot(N, L);
    if (NoL <= 0.0) {
        return vec3(0.0);
    }
    vec3 H = safeNormalize(V + L, N);
    float NoV = max(dot(N, V), 1e-4);
    float NoH = clamp(dot(N, H), 0.0, 1.0);
    float VoH = clamp(dot(V, H), 0.0, 1.0);

    float D = distributionGgx(NoH, surface.alpha);
    float visibility = visibilitySmithGgxCorrelated(NoV, NoL, surface.alpha);
    vec3 F = fresnelSchlick(surface.f0, VoH);
    vec3 specular = D * visibility * F;
    vec3 diffuse = surface.diffuseColor * diffuseLambert();
    return (diffuse + specular) * radiance * NoL;
}

// Directional albedo of the specular lobe integrated over a uniform environment: analytic fit of the split-sum
// environment BRDF (Karis 2014, "Physically Based Shading on Mobile").
vec3 environmentBrdf(vec3 f0, float perceptualRoughness, float NoV) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = perceptualRoughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 scaleBias = vec2(-1.04, 1.04) * a004 + r.zw;
    return f0 * scaleBias.x + scaleBias.y;
}
