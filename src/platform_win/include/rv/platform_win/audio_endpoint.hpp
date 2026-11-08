#pragma once

// Windows' default audio output (I11 step 14, ADR-0020 §6): the id of the render endpoint the system plays sounds on,
// which a game launched in VR plays on unless the headset is that device. Read only: Retroverse never changes the
// default device. GPU-free.

#include <string>

#include <rv/core/expected.hpp>

namespace rv::platform_win {

/// The id of Windows' default render endpoint for the console role (IMMDeviceEnumerator::GetDefaultAudioEndpoint;
/// "{0.0.0.00000000}.{guid}"), in UTF-8. NotFound when the system has no audio output.
[[nodiscard]] Expected<std::string> defaultAudioOutput();

} // namespace rv::platform_win
