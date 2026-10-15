// sRGB transfer functions (IEC 61966-2-1), as an sRGB target applies them when it encodes linear output
// (GL_FRAMEBUFFER_SRGB): the post pass dithers in the encoding the target quantizes in (post/post.frag), the inspection
// views write colors meant for display (debug/inspect.frag).

vec3 linearToSrgb(vec3 linear) {
    linear = clamp(linear, 0.0, 1.0);
    vec3 curve = 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055;
    return mix(curve, linear * 12.92, lessThanEqual(linear, vec3(0.0031308)));
}

vec3 srgbToLinear(vec3 encoded) {
    vec3 curve = pow((encoded + 0.055) / 1.055, vec3(2.4));
    return mix(curve, encoded / 12.92, lessThanEqual(encoded, vec3(0.04045)));
}
