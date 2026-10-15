#pragma once

// GPU-free: no OpenGL header is included. The binding points, layouts and constants below are shared with GLSL and
// pinned against the shader sources by tests/unit/integration/test_shader_sources.cpp.

#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace rv::gl {

/// Uniform buffer binding points: the single table for C++ and GLSL. Shaders declare their blocks with the same
/// numbers, `layout(std140, binding = N) uniform <Block> { ... };`:
///   0   Frame     FrameUniforms, per rendered view (assets/shaders/common/common.glsl)
///   1   Lights    LightsUniforms, scene lights and environment (assets/shaders/common/lights.glsl)
///   2   Material  MaterialUniforms, per material (assets/shaders/common/material.glsl)
///   3+  reserved: new blocks are added to this table first, then to the shaders.
enum class UniformBinding : std::uint32_t {
    Frame = 0,
    Lights = 1,
    Material = 2,
};

/// First binding point that is not assigned yet.
inline constexpr std::uint32_t kFirstReservedUniformBinding = 3;

[[nodiscard]] constexpr std::uint32_t toIndex(UniformBinding binding) noexcept {
    return static_cast<std::uint32_t>(binding);
}

static_assert(sizeof(glm::mat4) == 64 && sizeof(glm::vec3) == 12 && sizeof(glm::vec4) == 16,
              "glm types must be tightly packed (no GLM_FORCE_DEFAULT_ALIGNED_GENTYPES)");

// The C++ mirrors below follow std140 member for member: a mat4 is four 16-byte columns, a vec3 takes a 16-byte slot
// whose last 4 bytes the following scalar may use, a struct (and every array element) starts on a 16-byte boundary
// and is padded to a multiple of 16 bytes, and so is the whole block. Padding is explicit and every offset is
// checked, so a change on either side must be mirrored on the other.

/// Per-view constants at UniformBinding::Frame: block `FrameUniforms` of common/common.glsl.
struct FrameUniforms {
    glm::mat4 view{1.0f};           ///< world -> view
    glm::mat4 projection{1.0f};     ///< view -> clip (OpenGL clip space, depth in [-1, 1])
    glm::mat4 viewProjection{1.0f}; ///< projection * view
    glm::vec3 cameraPosition{0.0f}; ///< eye position in world space, meters
    float time = 0.0f;              ///< simulated seconds since the start of the run (the sum of the update steps)
};

static_assert(offsetof(FrameUniforms, view) == 0);
static_assert(offsetof(FrameUniforms, projection) == 64);
static_assert(offsetof(FrameUniforms, viewProjection) == 128);
static_assert(offsetof(FrameUniforms, cameraPosition) == 192);
static_assert(offsetof(FrameUniforms, time) == 204);
static_assert(sizeof(FrameUniforms) == 208, "std140 block sizes are multiples of 16 bytes");

/// Point and spot lights the shaders evaluate per fragment (RV_MAX_PUNCTUAL_LIGHTS in common/lights.glsl).
inline constexpr std::uint32_t kMaxPunctualLights = 8;

/// One point or spot light: struct `PunctualLight` of common/lights.glsl. Contract of rv::scene::Light (radiance =
/// color * intensity * attenuation * cone factor); the cone is precomputed so that one formula serves both kinds:
/// t = clamp(dot(direction, surface - position) / distance * spotScale + spotOffset, 0, 1), cone factor =
/// smoothstep(t), with spotScale = 1 / max(cos(inner) - cos(outer), 1e-4) and spotOffset = -cos(outer) * spotScale for
/// spot lights, and spotScale = 0, spotOffset = 1 (no cone) for point lights.
struct PunctualLightUniforms {
    glm::vec3 position{0.0f}; ///< world, meters
    float range = 1.0f;       ///< meters (> 0): the inverse-square falloff is windowed to reach 0 here
    glm::vec3 color{0.0f};    ///< linear RGB
    float intensity = 0.0f;   ///< scales color
    glm::vec3 direction{0.0f, -1.0f, 0.0f}; ///< unit vector the light travels along (spot axis)
    float spotScale = 0.0f;
    float spotOffset = 1.0f;
    float padding[3] = {0.0f, 0.0f, 0.0f};
};

static_assert(offsetof(PunctualLightUniforms, position) == 0);
static_assert(offsetof(PunctualLightUniforms, range) == 12);
static_assert(offsetof(PunctualLightUniforms, color) == 16);
static_assert(offsetof(PunctualLightUniforms, intensity) == 28);
static_assert(offsetof(PunctualLightUniforms, direction) == 32);
static_assert(offsetof(PunctualLightUniforms, spotScale) == 44);
static_assert(offsetof(PunctualLightUniforms, spotOffset) == 48);
static_assert(sizeof(PunctualLightUniforms) == 64, "std140 struct sizes are multiples of 16 bytes");

/// Scene lights and environment at UniformBinding::Lights: block `LightsUniforms` of common/lights.glsl. Built by
/// rv::gl::packLights (light_packing.hpp).
struct LightsUniforms {
    /// The first punctualLightCount entries are used; the rest are zero.
    PunctualLightUniforms punctualLights[kMaxPunctualLights]{};
    glm::vec3 directionalDirection{0.0f, -1.0f, 0.0f}; ///< unit vector the directional light travels along
    float directionalIntensity = 0.0f;                 ///< 0 when the scene has no directional light
    glm::vec3 directionalColor{0.0f};                  ///< linear RGB
    std::int32_t punctualLightCount = 0;
    glm::vec3 ambientSky{0.0f};    ///< linear RGB of the hemisphere above (+Y)
    float ambientIntensity = 0.0f; ///< scales the hemispheric sky/ground ambient
    glm::vec3 ambientGround{0.0f}; ///< linear RGB of the hemisphere below
    float exposure = 1.0f;         ///< multiplies the linear color before tonemapping
};

static_assert(offsetof(LightsUniforms, punctualLights) == 0);
static_assert(offsetof(LightsUniforms, directionalDirection) == 512);
static_assert(offsetof(LightsUniforms, directionalIntensity) == 524);
static_assert(offsetof(LightsUniforms, directionalColor) == 528);
static_assert(offsetof(LightsUniforms, punctualLightCount) == 540);
static_assert(offsetof(LightsUniforms, ambientSky) == 544);
static_assert(offsetof(LightsUniforms, ambientIntensity) == 556);
static_assert(offsetof(LightsUniforms, ambientGround) == 560);
static_assert(offsetof(LightsUniforms, exposure) == 572);
static_assert(sizeof(LightsUniforms) == 576, "std140 block sizes are multiples of 16 bytes");

/// Texture units of a material: `layout(binding = N) uniform sampler2D` in common/material.glsl. Units from
/// kMaterialTextureSlotCount on are free for other passes.
enum class MaterialTextureSlot : std::uint32_t {
    BaseColor = 0,         ///< sRGB; multiplies baseColorFactor
    MetallicRoughness = 1, ///< linear; G scales roughness, B scales metallic (glTF)
    Normal = 2,            ///< linear, tangent space, scaled by normalScale
    Occlusion = 3,         ///< linear; R, mixed in by occlusionStrength
    Emissive = 4,          ///< sRGB; multiplies the emissive color
};
inline constexpr std::uint32_t kMaterialTextureSlotCount = 5;

[[nodiscard]] constexpr std::uint32_t toIndex(MaterialTextureSlot slot) noexcept {
    return static_cast<std::uint32_t>(slot);
}

/// Bits of MaterialUniforms::flags (`RV_MATERIAL_*` constants of common/material.glsl). The texture bits tell which
/// slots hold a material texture; the shaders sample only those (the others hold neutral defaults).
namespace material_flags {
inline constexpr std::uint32_t kBaseColorTexture = 1u << 0;
inline constexpr std::uint32_t kMetallicRoughnessTexture = 1u << 1;
inline constexpr std::uint32_t kNormalTexture = 1u << 2;
inline constexpr std::uint32_t kOcclusionTexture = 1u << 3;
inline constexpr std::uint32_t kEmissiveTexture = 1u << 4;
/// Alpha mode mask: fragments whose alpha is below alphaCutoff are discarded.
inline constexpr std::uint32_t kAlphaMask = 1u << 5;
/// Alpha mode blend: the shader outputs the material alpha (otherwise 1).
inline constexpr std::uint32_t kAlphaBlend = 1u << 6;
/// Both faces are drawn; back faces shade with the flipped normal.
inline constexpr std::uint32_t kDoubleSided = 1u << 7;

/// Flag of the texture in `slot`.
[[nodiscard]] constexpr std::uint32_t textureBit(MaterialTextureSlot slot) noexcept {
    return 1u << toIndex(slot);
}
} // namespace material_flags

/// Per-material parameters at UniformBinding::Material: block `MaterialUniforms` of common/material.glsl. Built from
/// an assets::MaterialDesc by rv::gl::makeMaterialUniforms (material_uniforms.hpp). Colors are linear.
struct MaterialUniforms {
    glm::vec4 baseColorFactor{1.0f};
    glm::vec3 emissive{0.0f}; ///< emissive factor * emissive strength (PBR)
    float metallic = 0.0f;
    float roughness = 0.5f; ///< perceptual roughness
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float alphaCutoff = 0.5f;      ///< used with material_flags::kAlphaMask
    float emissiveStrength = 1.0f; ///< unlit materials: scales base color * texture
    std::uint32_t flags = 0;       ///< material_flags bits
    float padding[2] = {0.0f, 0.0f};
};

static_assert(offsetof(MaterialUniforms, baseColorFactor) == 0);
static_assert(offsetof(MaterialUniforms, emissive) == 16);
static_assert(offsetof(MaterialUniforms, metallic) == 28);
static_assert(offsetof(MaterialUniforms, roughness) == 32);
static_assert(offsetof(MaterialUniforms, normalScale) == 36);
static_assert(offsetof(MaterialUniforms, occlusionStrength) == 40);
static_assert(offsetof(MaterialUniforms, alphaCutoff) == 44);
static_assert(offsetof(MaterialUniforms, emissiveStrength) == 48);
static_assert(offsetof(MaterialUniforms, flags) == 52);
static_assert(sizeof(MaterialUniforms) == 64, "std140 block sizes are multiples of 16 bytes");

} // namespace rv::gl
