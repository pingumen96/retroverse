#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace rv::app {

/// Rendering checks a frame-limited run performs after initialization (--self-check <name>). A failed check fails
/// the run (ExitCode::RuntimeFailure).
enum class SelfCheck : std::uint8_t {
    /// sRGB output pipeline: an unlit quad of linear 0.5 grey, drawn with the unlit shader into the HDR target of view
    /// targets like the window's and the screenshots', then taken through the post pass to their sRGB output (bloom,
    /// vignette and dithering off, exposure 1, the ACES curve replaced by a clamp), must read back as the sRGB encoding
    /// of 0.5, 188, in every pixel and channel within kSrgbCheckTolerance.
    Srgb,
};

/// Command line name: "srgb".
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

} // namespace rv::app
