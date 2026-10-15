// Display transform shared by every shaded surface, lit or unlit (ADR-0002: exposure + ACES fitted curve). The
// result is linear in [0, 1]; the sRGB framebuffer encodes it (GL_FRAMEBUFFER_SRGB). Mirrored on the CPU, for the
// clear color, by rv::gl::tonemap (src/render/include/rv/render/tonemap.hpp): change both together.

// ACES fitted (Stephen Hill, BakingLab): sRGB -> ACES AP1-like space, the RRT + ODT curve fit, back to sRGB. The
// matrices are written column by column (GLSL mat3 constructors are column-major).
const mat3 RV_ACES_INPUT = mat3(0.59719, 0.07600, 0.02840,
                                0.35458, 0.90834, 0.13383,
                                0.04823, 0.01566, 0.83777);
const mat3 RV_ACES_OUTPUT = mat3(1.60475, -0.10208, -0.00327,
                                 -0.53108, 1.10813, -0.07276,
                                 -0.07367, -0.00605, 1.07602);

vec3 acesRrtOdtFit(vec3 v) {
    vec3 a = v * (v + 0.0245786) - 0.000090537;
    vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    return a / b;
}

vec3 tonemapAcesFitted(vec3 color) {
    color = RV_ACES_INPUT * color;
    color = acesRrtOdtFit(color);
    color = RV_ACES_OUTPUT * color;
    return clamp(color, 0.0, 1.0);
}

// Linear scene radiance -> linear display value: exposure, then the ACES fitted curve. Negative or non-finite input
// (which no light should produce) is clamped to black first, so a bad pixel never spreads through the curve.
vec3 tonemap(vec3 radiance, float exposure) {
    vec3 exposed = max(radiance * exposure, vec3(0.0));
    exposed = mix(exposed, vec3(0.0), isnan(exposed));
    return tonemapAcesFitted(min(exposed, vec3(65504.0)));
}
