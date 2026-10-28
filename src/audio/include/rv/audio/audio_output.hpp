#pragma once

// rv_audio_model (I5 step 5, ADR-0016 §9): GPU-free and without miniaudio, so that the GPU-free parts of the
// application (rv_app_cli) can name the outputs without linking the engine.

#include <cstdint>
#include <optional>
#include <string_view>

namespace rv::audio {

/// Where the mix goes (ADR-0016 §9, --audio).
enum class AudioOutput : std::uint8_t {
    Device, ///< miniaudio's shared-mode default endpoint: the interactive runs' default
    Null,   ///< no operating-system audio at all: the main thread pulls the mix (frame-limited runs, tests)
    Off,    ///< no mixer and no audio decoded
};

/// "device", "null", "off".
[[nodiscard]] std::string_view toString(AudioOutput output) noexcept;
[[nodiscard]] std::optional<AudioOutput> audioOutputFromName(std::string_view name) noexcept;

} // namespace rv::audio
