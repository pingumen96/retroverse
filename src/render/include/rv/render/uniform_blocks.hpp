#pragma once

// GPU-free: no OpenGL header is included. The binding points and layouts below are shared with GLSL.

#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace rv::gl {

/// Uniform buffer binding points: the single table for C++ and GLSL. Shaders declare their blocks with the same
/// numbers, `layout(std140, binding = N) uniform <Block> { ... };`:
///   0   Frame     FrameUniforms, per rendered view (assets/shaders/common/common.glsl)
///   1   Lights    light list (M0 step 6)
///   2   Material  per-material parameters (M0 steps 6-8)
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

/// Per-view constants at UniformBinding::Frame: the C++ mirror of the std140 block `FrameUniforms` in
/// assets/shaders/common/common.glsl, member for member. std140 places a mat4 as four 16-byte columns, gives a vec3 a
/// 16-byte slot whose last 4 bytes the following float may use, and rounds the block size up to 16 bytes; padding is
/// explicit and every offset is checked below, so a change on either side must be mirrored on the other.
struct FrameUniforms {
    glm::mat4 view{1.0f};           ///< world -> view
    glm::mat4 projection{1.0f};     ///< view -> clip (OpenGL clip space, depth in [-1, 1])
    glm::mat4 viewProjection{1.0f}; ///< projection * view
    glm::vec3 cameraPosition{0.0f}; ///< eye position in world space, meters
    float time = 0.0f;              ///< seconds since the start of the run
    float exposure = 1.0f;          ///< exposure of the scene environment, applied before tonemapping (step 6)
    float padding[3] = {0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(glm::mat4) == 64 && sizeof(glm::vec3) == 12,
              "glm types must be tightly packed (no GLM_FORCE_DEFAULT_ALIGNED_GENTYPES)");
static_assert(offsetof(FrameUniforms, view) == 0);
static_assert(offsetof(FrameUniforms, projection) == 64);
static_assert(offsetof(FrameUniforms, viewProjection) == 128);
static_assert(offsetof(FrameUniforms, cameraPosition) == 192);
static_assert(offsetof(FrameUniforms, time) == 204);
static_assert(offsetof(FrameUniforms, exposure) == 208);
static_assert(sizeof(FrameUniforms) == 224, "std140 block sizes are multiples of 16 bytes");

} // namespace rv::gl
