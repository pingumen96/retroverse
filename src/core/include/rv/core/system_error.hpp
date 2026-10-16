#pragma once

// Windows error codes (GetLastError) as text and as rv::ErrorCode, for the modules that call Win32 directly.

#include <cstdint>
#include <filesystem>
#include <string>

#include <rv/core/expected.hpp>

namespace rv::core {

/// "<system message> (error <code>)" for a Windows error code: the English message when the system has it, else the
/// user's language, never empty.
[[nodiscard]] std::string systemErrorMessage(std::uint32_t code);

/// systemErrorMessage(GetLastError()).
[[nodiscard]] std::string lastSystemErrorMessage();

/// The category of a Windows error code met while opening or listing files: NotFound (no such file or folder, an
/// invalid name), Unavailable (a drive that is not ready or not mapped, a share or device that cannot be reached),
/// Conflict (another program holds the file locked or open without sharing), IoError (access denied and every other
/// failure).
[[nodiscard]] ErrorCode fileErrorCode(std::uint32_t code) noexcept;

/// fileErrorCode() of a failure on `path`, where NotFound becomes Unavailable when the drive or the share the path
/// starts with does not exist at all: an unplugged or unmapped drive is not a deleted file (ADR-0008 §4).
[[nodiscard]] ErrorCode fileErrorCode(std::uint32_t code, const std::filesystem::path& path);

} // namespace rv::core
