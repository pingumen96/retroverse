// Ambient occlusion of a view as the lit shader (pbr/pbr.frag) reads it (rv::gl::SsaoPass): one value per pixel of the
// view's HDR target on texture unit 9 (rv::gl::SceneTextureUnit::AmbientOcclusion,
// src/render/include/rv/render/uniform_blocks.hpp), the unoccluded share of the ambient light there, 1 where nothing
// occludes it and in views without ambient occlusion. It darkens the ambient light only: the direct light of the key
// light has its shadow.

layout(binding = 9) uniform sampler2D uAmbientOcclusion;

// Ambient occlusion at the pixel being shaded (no filtering: the target has the size of the view).
float screenAmbientOcclusion() {
    return texelFetch(uAmbientOcclusion, ivec2(gl_FragCoord.xy), 0).r;
}

// Occlusion of the ambient specular lobe from the ambient occlusion `visibility` (Lagarde and de Rousiers 2014, "Moving
// Frostbite to PBR"; as in Filament): glossy surfaces seen head-on keep their reflection of the room in an occluded
// corner, rough ones and grazing views lose it like the diffuse light. `alpha` is the GGX roughness (perceptual
// roughness squared).
float specularOcclusion(float NoV, float visibility, float alpha) {
    return clamp(pow(NoV + visibility, exp2(-16.0 * alpha - 1.0)) - 1.0 + visibility, 0.0, 1.0);
}
