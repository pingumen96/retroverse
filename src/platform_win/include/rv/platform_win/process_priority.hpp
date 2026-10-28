#pragma once

// The process's priority class (I5 step 2, ADR-0016 §2; ADR-0004 point 9): above normal while Retroverse renders its
// 3D view, normal while minimized and before an emulator starts. Windows-only; no SDL.

#include <cstdint>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::platform_win {

enum class ProcessPriority : std::uint8_t {
    Normal,      ///< NORMAL_PRIORITY_CLASS
    AboveNormal, ///< ABOVE_NORMAL_PRIORITY_CLASS
    Other,       ///< any other class (read back only: set by someone else)
};

/// "normal", "above-normal", "other".
[[nodiscard]] std::string_view toString(ProcessPriority priority) noexcept;

/// The calling process's priority class now (GetPriorityClass).
[[nodiscard]] ProcessPriority currentProcessPriority() noexcept;

/// Sets the calling process's priority class (SetPriorityClass on the current process; its threads keep their
/// relative priorities). Errors: InvalidArgument for Other; PlatformError when the system refuses, with its message.
[[nodiscard]] Status setProcessPriority(ProcessPriority priority);

} // namespace rv::platform_win
