#pragma once

#include <rv/app/self_check.hpp>
#include <rv/core/expected.hpp>

namespace rv::gl {
class GLDevice;
class ShaderProgram;
class TextureDefaults;
struct PostShaders;
} // namespace rv::gl

namespace rv::app {

/// Runs SelfCheck::Fxaa on the GPU through the output path of every view, with the programs the views use: `unlit`
/// (pbr/pbr.vert + unlit/unlit_emissive.frag) draws a quad of an emitter so bright that the tonemap shows it as white
/// over the right half of kFxaaCheckSize x kFxaaCheckSize view targets, its left edge through the center slanted
/// kFxaaCheckEdgeDegrees from the vertical, over a black background; gl::PostProcessor takes it with `post` to the
/// sRGB output without bloom, vignette or dithering, once without and once with anti-aliasing. Both outputs are read
/// back like screenshots and evaluated with evaluateFxaaCheck(). Errors mean the check could not run (a program that
/// did not build, buffers, targets); a check that ran and failed is a result that did not pass. Main thread only, with
/// the context current; the default framebuffer is bound afterwards.
[[nodiscard]] Expected<FxaaCheckResult> runFxaaSelfCheck(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                                         const gl::ShaderProgram& unlit, const gl::PostShaders& post);

/// Slant of the check's edge from the vertical, degrees: steep enough that the stair-steps span several rows.
inline constexpr float kFxaaCheckEdgeDegrees = 12.0f;

} // namespace rv::app
