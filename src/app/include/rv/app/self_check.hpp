#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace rv::app {

/// Rendering checks a frame-limited run performs after initialization (--self-check <name>). A failed check fails
/// the run (ExitCode::RuntimeFailure).
enum class SelfCheck : std::uint8_t {
    /// sRGB output pipeline: an unlit quad of the grey radiance the ACES curve shows as linear 0.5, drawn with the
    /// scene's unlit program into the HDR target of view targets like the window's and the screenshots', then taken
    /// through the post-processing the views use to their sRGB output, must read back as the sRGB encoding of 0.5,
    /// 188, in every pixel and channel within kSrgbCheckTolerance: once with exposure 1 and bloom, vignette and
    /// dithering off, once with bloom and dithering on (a uniform field, which neither may change).
    Srgb,
    /// Bloom chain: known HDR patterns taken through the post-processing the views use must bloom as documented
    /// (evaluateBloomCheck): a lit surface below the bloom threshold not at all, an emitter with its energy kept, a
    /// single very bright pixel with the same glow wherever it falls and however bright it is.
    Bloom,
    /// Anti-aliasing pass (FXAA): a hard slanted edge between black and an emitter, taken through the post-processing
    /// without dithering, once without and once with anti-aliasing, must come out as documented (evaluateFxaaCheck):
    /// pixels along the edge get intermediate values, flat areas are copied exactly; and with the bloom on, the post
    /// pass's image written over the emitted light it reads equals the one written into the output
    /// (evaluateFxaaInPlaceWrite).
    Fxaa,
};

/// Every check, in documentation order.
inline constexpr SelfCheck kSelfChecks[] = {SelfCheck::Srgb, SelfCheck::Bloom, SelfCheck::Fxaa};

/// Command line name: "srgb", "bloom", "fxaa".
[[nodiscard]] std::string_view toString(SelfCheck check) noexcept;
/// The check named `name` (exact, lowercase), or nothing.
[[nodiscard]] std::optional<SelfCheck> selfCheckFromString(std::string_view name) noexcept;

/// Linear grey drawn by the sRGB check.
inline constexpr float kSrgbCheckLinearValue = 0.5f;
/// Largest accepted difference between a stored byte and the expected one (driver rounding).
inline constexpr int kSrgbCheckTolerance = 2;

/// Byte an sRGB render target stores for the linear value `linear`: round(255 * linearToSrgb(linear)), 188 for 0.5.
[[nodiscard]] int expectedSrgbByte(float linear) noexcept;

/// Outcome of the sRGB check over the read-back pixels.
struct SrgbCheckResult {
    bool passed = false;
    int expected = 0;          ///< expectedSrgbByte(kSrgbCheckLinearValue)
    int minimum = 0;           ///< smallest R, G or B byte over all pixels (0 without pixels)
    int maximum = 0;           ///< largest R, G or B byte over all pixels (0 without pixels)
    std::size_t pixelCount = 0;
};

/// Evaluates the read-back of the check quad, tightly packed RGBA8 pixels in any row order (alpha is ignored): it
/// passes when there is at least one pixel and every R, G and B byte is within kSrgbCheckTolerance of the expected
/// byte. A trailing partial pixel is ignored.
[[nodiscard]] SrgbCheckResult evaluateSrgbCheck(std::span<const std::uint8_t> rgbaPixels) noexcept;

/// Largest accepted deviation of the bloom energy of an emitter from the energy of its light (a share of it): the
/// Karis average of the first level keeps a little less at the edges of bright areas.
inline constexpr float kBloomCheckEnergyTolerance = 0.05f;
/// Largest accepted deviation of the bloom energy of each single bright pixel from their mean (a share of it).
inline constexpr float kBloomCheckFireflyTolerance = 0.2f;

/// What SelfCheck::Bloom measured on the GPU. Bloom energy: the luminance of the largest bloom level summed over its
/// texels, times the view pixels each texel covers.
struct BloomCheckMeasurements {
    /// Largest component of the largest bloom level for a lit square just below the knee of the bloom threshold.
    float litBloomPeak = 0.0f;
    /// Bloom energy of an emitter square over the energy of its emitted light.
    float emitterEnergyRatio = 0.0f;
    /// Bloom energy of a single very bright reflected pixel, for several values and places under the pixel grid.
    std::vector<float> fireflyEnergies;
};

/// Outcome of the bloom check.
struct BloomCheckResult {
    bool passed = false;
    bool litStaysOut = false; ///< litBloomPeak is 0: lit surfaces below the threshold never bloom
    bool energyKept = false;  ///< emitterEnergyRatio within kBloomCheckEnergyTolerance of 1
    /// At least two firefly energies, every one within kBloomCheckFireflyTolerance of their mean.
    bool firefliesSteady = false;
    /// Largest deviation of a firefly energy from their mean, as a share of the mean (0 without energies).
    float fireflySpread = 0.0f;
};

/// Evaluates the bloom measurements: the check passes when lit surfaces stay out of the bloom, the emitter's energy is
/// kept and the fireflies are steady. Non-finite measurements fail.
[[nodiscard]] BloomCheckResult evaluateBloomCheck(const BloomCheckMeasurements& measurements) noexcept;

/// Side of the square target of SelfCheck::Fxaa, in pixels.
inline constexpr int kFxaaCheckSize = 64;
/// Smallest share of the rows crossed by the edge that must hold a pixel with an intermediate value after the
/// anti-aliasing pass. FXAA blends each pixel of a stair-step by its distance from the step's nearer end, so the middle
/// row of a step may keep its hard values.
inline constexpr float kFxaaCheckMinBlendedShare = 0.5f;

/// Outcome of the FXAA check over the two read-backs of the edge: without anti-aliasing ("aliased") and with it.
struct FxaaCheckResult {
    bool passed = false;
    /// Without anti-aliasing every pixel holds one of two colors (a hard edge, no dithering): the check's premise.
    bool aliasedIsHard = false;
    /// Rows of the aliased image that hold both colors (the edge crosses them).
    std::size_t edgeRows = 0;
    /// Of those, rows of the anti-aliased image with at least one pixel strictly between the two colors.
    std::size_t blendedEdgeRows = 0;
    /// blendedEdgeRows is at least kFxaaCheckMinBlendedShare of edgeRows, and edgeRows is not 0.
    bool edgeBlended = false;
    /// Pixels whose 3x3 neighborhood is a single color in the aliased image (flat areas), and how many of them the
    /// anti-aliasing changed.
    std::size_t flatPixels = 0;
    std::size_t flatChanged = 0;
    /// flatPixels is not 0 and flatChanged is 0: flat areas are copied exactly.
    bool flatCopied = false;
    /// The in-place write (evaluateFxaaInPlaceWrite): the edge taken through the post-processing with its bloom, the
    /// post pass's image written over the emitted light it reads (anti-aliasing on, gl::viewTargetPlan()) against the
    /// one written into the output (anti-aliasing off): its pixels, and how many of them differ in any byte.
    std::size_t inPlacePixels = 0;
    std::size_t inPlaceChanged = 0;
    /// inPlacePixels is not 0 and inPlaceChanged is 0.
    bool inPlaceExact = false;
};

/// Evaluates the FXAA check: `aliased` and `antialiased` are tightly packed RGBA8 read-backs of a `width` x `height`
/// target, rows in the same order (alpha is ignored). The two colors are the darkest and the brightest pixel of the
/// aliased image; it passes when the aliased image is hard, its edge is blended and its flat areas are copied exactly.
/// Read-backs smaller than width x height pixels fail.
[[nodiscard]] FxaaCheckResult evaluateFxaaCheck(std::span<const std::uint8_t> aliased,
                                                std::span<const std::uint8_t> antialiased, int width,
                                                int height) noexcept;
/// Adds the in-place write to `result`: `direct` (the post pass into the output) and `inPlace` (the post pass over the
/// emitted light's storage), tightly packed RGBA8 read-backs of a `width` x `height` target, must be equal byte for
/// byte, alpha (the luma the anti-aliasing reads) included; `result` passes only if they are. Read-backs smaller than
/// width x height pixels fail.
void evaluateFxaaInPlaceWrite(FxaaCheckResult& result, std::span<const std::uint8_t> direct,
                              std::span<const std::uint8_t> inPlace, int width, int height) noexcept;

} // namespace rv::app
