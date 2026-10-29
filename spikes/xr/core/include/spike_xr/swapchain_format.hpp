#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace rv::spikes::xr {

/// OpenGL internal formats a runtime may offer for XR_KHR_opengl_enable swapchains (the values of the GL enums: this
/// part of the spike includes no GL header).
inline constexpr std::int64_t kGlSrgb8Alpha8 = 0x8C43;
inline constexpr std::int64_t kGlRgba8 = 0x8058;
inline constexpr std::int64_t kGlRgb10A2 = 0x8059;
inline constexpr std::int64_t kGlRgba16f = 0x881A;

/// The color format the eyes render into.
struct ColorFormatChoice {
    std::int64_t format = 0;
    /// The runtime decodes the images as sRGB: the shader's linear output is encoded on write (GL_FRAMEBUFFER_SRGB),
    /// like the renderer's own `output` target. With a linear format the output is written as it is.
    bool srgb = false;
};

/// The format for the eyes among those the runtime offers (xrEnumerateSwapchainFormats): GL_SRGB8_ALPHA8 when offered
/// (the renderer's output format), else GL_RGBA8; nothing when neither is.
[[nodiscard]] std::optional<ColorFormatChoice> chooseColorFormat(std::span<const std::int64_t> offered) noexcept;

/// "GL_SRGB8_ALPHA8", ..., or the value in hexadecimal for a format this list does not name.
[[nodiscard]] std::string glFormatName(std::int64_t format);

} // namespace rv::spikes::xr
