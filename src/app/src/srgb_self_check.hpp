#pragma once

#include <filesystem>

#include <rv/app/self_check.hpp>
#include <rv/core/expected.hpp>

namespace rv::gl {
class GLDevice;
class TextureDefaults;
} // namespace rv::gl

namespace rv::app {

/// Runs SelfCheck::Srgb on the GPU through the output path of every view: the unlit program (pbr/pbr.vert +
/// unlit/unlit_emissive.frag, as the scene uses it) draws a full-target quad with an unlit material of linear grey
/// kSrgbCheckLinearValue (through the material uniform block, like every material) into the HDR target of 8x8 view
/// targets (gl::ViewTargets, like the window's and the screenshots'), then gl::PostProcessor takes it to the sRGB
/// output with bloom, vignette and dithering off and exposure 1, using the post program built from `shaderRoot` with
/// RV_POST_LINEAR_OUTPUT, which replaces the ACES curve with a clamp so that the output is the linear value itself. The
/// output is read back like a screenshot and its bytes are evaluated with evaluateSrgbCheck(). Errors mean the check
/// could not run (shaders, buffers, targets); a check that ran and failed is a result with passed false. Main thread
/// only, with the context current; the default framebuffer is bound afterwards.
[[nodiscard]] Expected<SrgbCheckResult> runSrgbSelfCheck(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                                         const std::filesystem::path& shaderRoot);

} // namespace rv::app
