#include <rv/audio/audio_output.hpp>

namespace rv::audio {

std::string_view toString(AudioOutput output) noexcept {
    switch (output) {
    case AudioOutput::Device:
        return "device";
    case AudioOutput::Null:
        return "null";
    case AudioOutput::Off:
        return "off";
    }
    return "off";
}

std::optional<AudioOutput> audioOutputFromName(std::string_view name) noexcept {
    if (name == "device") {
        return AudioOutput::Device;
    }
    if (name == "null") {
        return AudioOutput::Null;
    }
    if (name == "off") {
        return AudioOutput::Off;
    }
    return std::nullopt;
}

} // namespace rv::audio
