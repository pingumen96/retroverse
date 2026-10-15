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

/// Outcomes of the two passes of SelfCheck::Srgb.
struct SrgbSelfCheckResults {
    /// Exposure 1 and bloom, vignette and dithering off: the GPU's ACES curve against its CPU mirror and the encoding.
    SrgbCheckResult plain;
    /// Bloom (full strength) and dithering on as in a scene: on a uniform field neither may change the output.
    SrgbCheckResult postProcessed;

    [[nodiscard]] bool passed() const noexcept { return plain.passed && postProcessed.passed; }
};

/// Runs SelfCheck::Srgb on the GPU through the output path of every view, with the programs the views use: `unlit`
/// (pbr/pbr.vert + unlit/unlit_emissive.frag) draws a full-target quad with an unlit material of the grey radiance that
/// gl::tonemap() shows as linear kSrgbCheckLinearValue (gl::greyRadianceForDisplay) into the HDR target of 8x8 view
/// targets (gl::ViewTargets, like the window's and the screenshots'), then gl::PostProcessor takes it with `post` (the
/// bloom programs and the post program) to the sRGB output, once per pass of SrgbSelfCheckResults. Each output is read
/// back like a screenshot and evaluated with evaluateSrgbCheck(). Errors mean the check could not run (a program that
/// did not build, buffers, targets); a check that ran and failed is a result that did not pass. Main thread only, with
/// the context current; the default framebuffer is bound afterwards.
[[nodiscard]] Expected<SrgbSelfCheckResults> runSrgbSelfCheck(gl::GLDevice& device, const gl::TextureDefaults& defaults,
                                                              const gl::ShaderProgram& unlit,
                                                              const gl::PostShaders& post);

} // namespace rv::app
