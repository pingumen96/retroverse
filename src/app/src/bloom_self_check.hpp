#pragma once

#include <rv/app/self_check.hpp>
#include <rv/core/expected.hpp>

namespace rv::gl {
class GLDevice;
struct PostShaders;
} // namespace rv::gl

namespace rv::app {

/// Runs SelfCheck::Bloom on the GPU with the bloom and post programs the views use (`post`) and returns what it
/// measured, for evaluateBloomCheck(). Known patterns are written straight into the two attachments of the HDR target
/// of 512x512 view targets (gl::ViewTargets: the radiance and its emitted part, by scissored clears), taken through
/// gl::PostProcessor with bloom on (exposure 1, no vignette or dithering), and the largest bloom level is read back as
/// floats (gl::Framebuffer::readLinearPixels):
///  - a 64x64 lit square of reflected radiance just below the knee of the bloom threshold (gl::kBloomThreshold -
///    gl::kBloomKnee) on a dim lit background: the peak of its bloom;
///  - a 64x64 emitter square of radiance 0.5, all of it emitted, on the same background: its bloom energy over the
///    energy of its light;
///  - a single reflected pixel 100 and 1000 times brighter than white on black, at each of the four places of a 2x2
///    block of the pixel grid (which the first bloom level halves): its bloom energy.
/// Errors mean the check could not run (a bloom program that did not build, targets, readbacks). Main thread only,
/// with the context current; the default framebuffer is bound afterwards.
[[nodiscard]] Expected<BloomCheckMeasurements> runBloomSelfCheck(gl::GLDevice& device, const gl::PostShaders& post);

} // namespace rv::app
