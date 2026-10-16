#pragma once

// GPU-free (part of rv_cli_core): the output contract of retroverse-cli (ADR-0008 §2).

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::core {
class ArgParser;
class ParsedArgs;
} // namespace rv::core

namespace rv::cli {

/// Where text written by retroverse-cli goes. The output contract: a command's results go to stdout, or to the file
/// its --output option names, as UTF-8 text with '\n' line ends; diagnostics (usage errors, "not implemented yet") go
/// to stderr; logs never go to stdout: they go to stderr and to <user data>/logs/retroverse-cli.log.
class Output {
public:
    enum class Target {
        Stdout, ///< written through as it comes (binary mode while a ConsoleSession lives)
        Stderr, ///< written through as it comes
        File,   ///< kept in memory and written by finish(), atomically: a failed command leaves no partial file
        Memory, ///< kept in memory (tests)
    };

    [[nodiscard]] static Output standardOutput();
    [[nodiscard]] static Output standardError();
    /// Results for `path`, written by finish().
    [[nodiscard]] static Output file(std::filesystem::path path);
    [[nodiscard]] static Output memory();

    Output(Output&&) noexcept = default;
    Output& operator=(Output&&) noexcept = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    ~Output() = default;

    /// Appends UTF-8 text.
    void write(std::string_view utf8);
    /// Appends UTF-8 text and '\n'.
    void writeLine(std::string_view utf8 = {});

    /// Completes the output: flushes a standard stream, or writes a File target to its path (parent directories
    /// created, the file replaced atomically). Errors (IoError): a write to the stream or the file failed.
    [[nodiscard]] Status finish();

    [[nodiscard]] Target target() const noexcept { return m_target; }
    /// The file of a File target; empty otherwise.
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    /// What a File or Memory target holds; always empty for the standard streams, which keep nothing.
    [[nodiscard]] const std::string& text() const noexcept { return m_text; }

private:
    Output(Target target, std::filesystem::path path);

    Target m_target = Target::Memory;
    std::filesystem::path m_path;
    std::string m_text;
    bool m_writeFailed = false; ///< a write to a standard stream failed; finish() reports it
};

/// Declares a command's --output <file> option: the results go to that file instead of stdout.
void addOutputOption(core::ArgParser& parser);

/// The File output a command's --output option names; nothing when the option was not given (the results go to the
/// run's stdout). Errors (InvalidArgument): an empty value.
[[nodiscard]] Expected<std::optional<Output>> outputFileOption(const core::ParsedArgs& args);

/// The console of a retroverse-cli process, set up for the output contract while it lives (main() holds one): stdout
/// in binary mode, so that results reach it byte for byte with '\n' line ends, and, when stdout or stderr is a console,
/// the console's output code page switched to UTF-8 so that UTF-8 results and logs show as such. The previous code page
/// comes back when it is destroyed, and when Ctrl+C, Ctrl+Break or closing the console ends the process.
class ConsoleSession {
public:
    ConsoleSession();
    ~ConsoleSession();

    ConsoleSession(const ConsoleSession&) = delete;
    ConsoleSession& operator=(const ConsoleSession&) = delete;

    /// True when the console's output code page was switched to UTF-8 (and is restored at the end).
    [[nodiscard]] bool switchedCodePage() const noexcept { return m_previousCodePage != 0; }

private:
    unsigned m_previousCodePage = 0; ///< 0: unchanged
    int m_previousStdoutMode = -1;   ///< -1: unchanged
};

} // namespace rv::cli
