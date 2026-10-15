#pragma once

#include <filesystem>

#include <rv/app/self_check.hpp>
#include <rv/core/expected.hpp>

namespace rv::gl {
class GLDevice;
class TextureDefaults;
} // namespace rv::gl

namespace rv::app {

/// Runs SelfCheck::Srgb on the GPU: builds the unlit program (pbr/pbr.vert + unlit/unlit_emissive.frag) from
/// `shaderRoot` with RV_UNLIT_LINEAR_OUTPUT, so that exposure and tonemapping are skipped, draws a full-target quad
/// with an unlit material of linear grey kSrgbCheckLinearValue (through the material uniform block, like every
/// material) into an 8x8 sRGB framebuffer prepared like the window and screenshot targets (GLDevice::beginPass:
/// GL_FRAMEBUFFER_SRGB on), reads it back like a screenshot and evaluates the bytes with evaluateSrgbCheck(). Errors
/// mean the check could not run (shader, buffers, target); a check that ran and failed is a result with passed false.
/// Main thread only, with the context current; the default framebuffer is bound afterwards.
[[nodiscard]] Expected<SrgbCheckResult> runSrgbSelfCheck(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                                         const std::filesystem::path& shaderRoot);

} // namespace rv::app
