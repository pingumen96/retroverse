// CRT_Subtle (style §15.2, §25): the picture of an unlit screen material shown the way a picture tube shows it, on the
// emissive path of unlit/unlit_emissive.frag, with the crt* parameters of the material block (common/material.glsl,
// RV_MATERIAL_CRT_SCREEN; rv::assets::CrtEffect):
//  - a barrel curvature: the picture bows out and its corners round off into the tube's dark border;
//  - scanlines and an RGB aperture grille that keep the mean brightness and fade out as they shrink below a few pixels,
//    so the picture stays legible and shows no moiré at a distance;
//  - a vignette towards the picture's edges;
//  - the glass in front of it, a smooth dielectric that reflects the scene's lights and ambient a little.
// Needs common/common.glsl, common/material.glsl, common/lights.glsl and common/brdf.glsl.

// Perceptual roughness of the screen's glass: glossy, with the slight haze of an anti-glare coating.
const float RV_CRT_GLASS_ROUGHNESS = 0.2;

// Picture coordinates of the screen coordinates `uv` under the barrel curvature. Near the corners they leave [0, 1]:
// the tube's border shows there.
vec2 crtPictureUv(vec2 uv) {
    vec2 centered = uv * 2.0 - 1.0;
    centered *= 1.0 + uCrtCurvature * centered.yx * centered.yx;
    return centered * 0.5 + 0.5;
}

// Share of a ripple of `period` positions that survives at a footprint of `footprint` positions per pixel: all of it
// from 4 pixels per period on, nothing below 2 (where sampling would turn it into moiré).
float crtRippleFade(float footprint) {
    return 1.0 - smoothstep(0.25, 0.5, footprint);
}

// Light of the tube at the picture coordinates `picture`, a multiplier of the picture's color: scanlines, the phosphor
// mask, the vignette, and the dark border outside the picture (anti-aliased over a pixel). Uses derivatives: call it
// in uniform control flow, before any discard.
vec3 crtPictureLight(vec2 picture) {
    // Scanlines: brightest on each line's center, darkest between lines; mean 1.
    float line = picture.y * uCrtLineCount;
    float scanlines = 1.0 - 0.5 * uCrtScanlineStrength * crtRippleFade(fwidth(line)) * cos(2.0 * RV_PI * line);
    // Aperture grille: red, green and blue stripes a third of a triad apart; each channel's mean is 1.
    float triad = picture.x * uCrtMaskTriadCount;
    vec3 stripes = cos(2.0 * RV_PI * (triad - vec3(0.0, 1.0 / 3.0, 2.0 / 3.0)));
    vec3 mask = 1.0 + uCrtMaskStrength * crtRippleFade(fwidth(triad)) * stripes;
    // Vignette: 1 in the middle, falling towards the edges.
    vec2 inside = clamp(picture * (1.0 - picture) * 4.0, 0.0, 1.0);
    float vignette = mix(1.0, pow(inside.x * inside.y, 0.25), uCrtVignette);
    // The picture ends at its curved edge; beyond it the tube is dark.
    vec2 outside = max(-picture, picture - 1.0);
    float pixel = max(max(fwidth(picture.x), fwidth(picture.y)), 1e-6);
    float coverage = 1.0 - smoothstep(-pixel, pixel, max(outside.x, outside.y));
    return mask * (scanlines * vignette * coverage);
}

// Radiance the screen's glass reflects towards the eye at `position` with the geometric `normal`: the scene's lights
// and the hemisphere ambient mirrored by a smooth dielectric (4% at normal incidence), times uCrtGlassReflection.
vec3 crtGlassReflection(vec3 position, vec3 normal) {
    vec3 N = safeNormalize(normal, vec3(0.0, 0.0, 1.0));
    vec3 V = safeNormalize(uCameraPosition - position, N);
    BrdfSurface glass = makeBrdfSurface(vec3(0.0), 0.0, RV_CRT_GLASS_ROUGHNESS);
    vec3 reflected = vec3(0.0);
    int lightCount = min(uPunctualLightCount, RV_MAX_PUNCTUAL_LIGHTS);
    for (int i = 0; i < lightCount; ++i) {
        vec3 L;
        vec3 incoming = punctualLightRadiance(i, position, L);
        reflected += evaluateLight(glass, N, V, L, incoming);
    }
    if (uDirectionalIntensity > 0.0) {
        reflected += evaluateLight(glass, N, V, -uDirectionalDirection, uDirectionalColor * uDirectionalIntensity);
    }
    float NoV = max(dot(N, V), 1e-4);
    reflected += hemisphereAmbient(reflect(-V, N)) * environmentBrdf(glass.f0, glass.perceptualRoughness, NoV);
    return reflected * uCrtGlassReflection;
}
