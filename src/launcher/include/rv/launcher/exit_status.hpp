#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rv::launcher {

/// What the exit code of a Windows process says about how it ended.
enum class ExitKind : std::uint8_t {
    Success,     ///< 0
    ErrorCode,   ///< a non-zero code the program chose (exit(1), exit(-1) = 0xFFFFFFFF, abort() = 3, ...)
    Crash,       ///< an unhandled exception: an NTSTATUS error code such as 0xC0000005 (access violation), 0xC0000409
                 ///< (fail fast), 0x80000003 (breakpoint), or a C++ / CLR exception code (0xE06D7363, 0xE0434352)
    LoaderError, ///< the program could not be loaded: 0xC0000135 (DLL not found), 0xC000007B (bad image format), ...
    Interrupted, ///< 0xC000013A (Ctrl+C or its console closed), 0x40010004 (terminated by a debugger)
};

[[nodiscard]] std::string_view toString(ExitKind kind) noexcept;

/// An exit code with its classification.
struct ExitStatus {
    std::uint32_t code = 0;
    ExitKind kind = ExitKind::Success;
    /// The NTSTATUS name of a known code ("STATUS_ACCESS_VIOLATION"), empty otherwise.
    std::string_view name;
};

/// Classifies an exit code (GetExitCodeProcess). Known exception and loader codes come from a table; other values of
/// the form 0xC000xxxx (NTSTATUS errors of the system facility, which processes end with only through an unhandled
/// exception) count as crashes; every other non-zero value is a program's own error code.
[[nodiscard]] ExitStatus classifyExitCode(std::uint32_t code) noexcept;

/// "0 (success)", "1 (error code)", "0xC0000005 STATUS_ACCESS_VIOLATION (crash)": codes from 0x10000 up in hexadecimal.
[[nodiscard]] std::string describeExitCode(std::uint32_t code);

} // namespace rv::launcher
