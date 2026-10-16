#include <rv/cli/output.hpp>

#include <atomic>
#include <cstdio>
#include <exception>
#include <utility>

#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <rv/core/arg_parser.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

namespace rv::cli {
namespace {

/// The code page a ConsoleSession switched away from, for the console control handler; 0 when nothing to restore.
std::atomic<unsigned> g_codePageToRestore{0};

/// Ctrl+C, Ctrl+Break, closing the console, logoff, shutdown: the code page comes back, then the default handler ends
/// the process (FALSE: not handled here).
BOOL WINAPI restoreCodePageOnExit(DWORD /*event*/) {
    if (const unsigned codePage = g_codePageToRestore.exchange(0); codePage != 0) {
        SetConsoleOutputCP(codePage);
    }
    return FALSE;
}

bool isConsole(DWORD standardHandle) {
    const HANDLE handle = GetStdHandle(standardHandle);
    DWORD mode = 0;
    return handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0;
}

std::FILE* streamOf(Output::Target target) {
    return target == Output::Target::Stdout ? stdout : stderr;
}

} // namespace

// --- Output --------------------------------------------------------------------------------------------------------

Output::Output(Target target, std::filesystem::path path) : m_target(target), m_path(std::move(path)) {}

Output Output::standardOutput() {
    return Output(Target::Stdout, {});
}

Output Output::standardError() {
    return Output(Target::Stderr, {});
}

Output Output::file(std::filesystem::path path) {
    return Output(Target::File, std::move(path));
}

Output Output::memory() {
    return Output(Target::Memory, {});
}

void Output::write(std::string_view utf8) {
    switch (m_target) {
    case Target::Stdout:
    case Target::Stderr:
        if (!utf8.empty() && std::fwrite(utf8.data(), 1, utf8.size(), streamOf(m_target)) != utf8.size()) {
            m_writeFailed = true;
        }
        break;
    case Target::File:
    case Target::Memory:
        m_text.append(utf8);
        break;
    }
}

void Output::writeLine(std::string_view utf8) {
    write(utf8);
    write("\n");
}

Status Output::finish() {
    switch (m_target) {
    case Target::Stdout:
    case Target::Stderr: {
        const bool flushed = std::fflush(streamOf(m_target)) == 0;
        if (m_writeFailed || !flushed) {
            return makeError(ErrorCode::IoError, "writing failed", m_target == Target::Stdout ? "stdout" : "stderr");
        }
        return ok();
    }
    case Target::File:
        return core::writeTextFileAtomic(m_path, m_text);
    case Target::Memory:
        return ok();
    }
    return ok();
}

void addOutputOption(core::ArgParser& parser) {
    parser.option("output", "file", "Write the results to this file (UTF-8) instead of stdout.");
}

Expected<std::optional<Output>> outputFileOption(const core::ParsedArgs& args) {
    if (!args.has("output")) {
        return std::optional<Output>{};
    }
    const std::string text = args.value("output").value_or("");
    if (text.empty()) {
        return makeError(ErrorCode::InvalidArgument, "value must not be empty", "--output");
    }
    try {
        return std::optional<Output>(Output::file(core::pathFromUtf8(text)));
    } catch (const std::exception&) {
        // pathFromUtf8 throws on malformed UTF-8.
        return makeError(ErrorCode::InvalidArgument, "value is not valid UTF-8", "--output");
    }
}

// --- ConsoleSession ------------------------------------------------------------------------------------------------

ConsoleSession::ConsoleSession() {
    // Binary mode: '\n' reaches stdout as it is (text mode would write "\r\n" into redirected results).
    if (const int descriptor = _fileno(stdout); descriptor >= 0) {
        std::fflush(stdout);
        m_previousStdoutMode = _setmode(descriptor, _O_BINARY);
    }
    // The output code page belongs to the console, which stdout and stderr share.
    if (isConsole(STD_OUTPUT_HANDLE) || isConsole(STD_ERROR_HANDLE)) {
        const UINT current = GetConsoleOutputCP();
        if (current != 0 && current != CP_UTF8 && SetConsoleOutputCP(CP_UTF8) != 0) {
            m_previousCodePage = current;
            g_codePageToRestore.store(current);
            SetConsoleCtrlHandler(restoreCodePageOnExit, TRUE);
        }
    }
}

ConsoleSession::~ConsoleSession() {
    std::fflush(stdout);
    std::fflush(stderr);
    if (m_previousCodePage != 0) {
        SetConsoleCtrlHandler(restoreCodePageOnExit, FALSE);
        if (g_codePageToRestore.exchange(0) != 0) {
            SetConsoleOutputCP(m_previousCodePage);
        }
    }
    if (const int descriptor = _fileno(stdout); descriptor >= 0 && m_previousStdoutMode != -1) {
        _setmode(descriptor, m_previousStdoutMode);
    }
}

} // namespace rv::cli
