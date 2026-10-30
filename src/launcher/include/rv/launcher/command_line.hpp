#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <rv/core/expected.hpp>
#include <rv/core/utf8.hpp>

namespace rv::launcher {

/// The longest command line CreateProcessW accepts: 32767 UTF-16 code units, the terminating null included.
inline constexpr std::size_t kMaxCommandLineUnits = 32766;

/// True when `text` is well-formed UTF-8: no stray continuation bytes, no overlong forms, no UTF-16 surrogates, nothing
/// above U+10FFFF. NUL bytes are well-formed (the command line rejects them separately). rv::core's check.
using core::isValidUtf8;

/// UTF-16 code units of well-formed UTF-8 `text`: one per code point, two above U+FFFF. Meaningless for malformed text.
[[nodiscard]] std::size_t utf16Length(std::string_view text) noexcept;

/// Appends `argument` to `commandLine` so that CommandLineToArgvW, and the C runtime of the started program, give it
/// back verbatim as one argument (specification §11.4: arguments are never interpolated by a shell):
///  - left as it is when it is not empty and holds no space, tab, newline, vertical tab or double quote;
///  - otherwise in double quotes, where each double quote is preceded by a backslash and the backslashes in front of a
///    double quote, or of the closing quote, are doubled (backslashes anywhere else are literal).
/// Works on UTF-8 bytes: the characters that matter are ASCII, and UTF-8 never encodes other characters with ASCII
/// bytes. NUL bytes are not checked here (buildCommandLine rejects them). Never use the result with cmd.exe, whose
/// own parsing (`%`, `^`, `&`, `|`, `<`, `>`) differs.
void appendQuotedArgument(std::string& commandLine, std::string_view argument);

/// appendQuotedArgument()'s quoted form whatever `argument` holds (I5 step 8's review): for a program that reads its
/// command line its own way, where an unquoted argument splits at other characters than CommandLineToArgvW's
/// (explorer.exe at its commas). CommandLineToArgvW gives it back verbatim just the same.
void appendAlwaysQuotedArgument(std::string& commandLine, std::string_view argument);

/// appendQuotedArgument() into a new string.
[[nodiscard]] std::string quoteWindowsArgument(std::string_view argument);

/// How buildCommandLine() quotes the arguments.
enum class ArgumentQuoting : std::uint8_t {
    WhenNeeded, ///< appendQuotedArgument()
    Always,     ///< appendAlwaysQuotedArgument()
};

/// The command line of a process: `program` (argv[0]) followed by every argument quoted by appendQuotedArgument() (or
/// appendAlwaysQuotedArgument(): `quoting`), separated by one space. The program name is always put in double quotes, as
/// CommandLineToArgvW reads argv[0] up to the next double quote without escape sequences (so a program path ending in a
/// backslash stays intact). Inverse of CommandLineToArgvW for every list of arguments it accepts (tested against it in
/// tests/unit/launcher). Errors (ValidationFailed, context naming the offending item): an empty program name, a program
/// name with a double quote (no Windows path holds one), NUL characters or malformed UTF-8 anywhere, a result longer
/// than kMaxCommandLineUnits UTF-16 code units.
[[nodiscard]] Expected<std::string> buildCommandLine(std::string_view program, std::span<const std::string> arguments,
                                                     ArgumentQuoting quoting = ArgumentQuoting::WhenNeeded);

} // namespace rv::launcher
