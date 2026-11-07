#include <rv/launcher/launch_request.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <system_error>

#include <fmt/format.h>

#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>

namespace rv::launcher {
namespace {

/// pathToUtf8 throws on paths that UTF-8 cannot represent (unpaired UTF-16 surrogates on Windows).
[[nodiscard]] Expected<std::string> utf8Path(const std::filesystem::path& path, std::string_view field) {
    try {
        return core::pathToUtf8(path);
    } catch (const std::exception&) {
        return makeError(ErrorCode::ValidationFailed, "the path cannot be represented in UTF-8", std::string(field));
    }
}

/// The executable is a program CreateProcessW can run: a PE image (an "MZ" header pointing at a "PE\0\0" signature)
/// for x64, x86 or ARM64. DOS and 16-bit programs, which 64-bit Windows refuses with a modal message box, and files
/// that are not programs at all are refused here, with a message that says why.
Status checkWindowsProgram(const std::filesystem::path& path, const std::string& shown) {
    std::ifstream file(path, std::ios::binary);
    std::array<unsigned char, 0x40> header{};
    if (!file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size())) ||
        header[0] != 'M' || header[1] != 'Z') {
        return makeError(ErrorCode::Unsupported, "the executable is not a Windows program", "executable: " + shown);
    }
    const std::uint32_t peOffset = static_cast<std::uint32_t>(header[0x3C]) |
                                   (static_cast<std::uint32_t>(header[0x3D]) << 8) |
                                   (static_cast<std::uint32_t>(header[0x3E]) << 16) |
                                   (static_cast<std::uint32_t>(header[0x3F]) << 24);
    std::array<unsigned char, 6> signature{};
    if (peOffset < header.size() || peOffset > (16u << 20) || !file.seekg(peOffset) ||
        !file.read(reinterpret_cast<char*>(signature.data()), static_cast<std::streamsize>(signature.size())) ||
        signature[0] != 'P' || signature[1] != 'E' || signature[2] != 0 || signature[3] != 0) {
        return makeError(ErrorCode::Unsupported,
                         "the executable is a DOS or 16-bit program, which 64-bit Windows does not run",
                         "executable: " + shown);
    }
    const auto machine = static_cast<std::uint16_t>(signature[4] | (signature[5] << 8));
    constexpr std::uint16_t kX64 = 0x8664;
    constexpr std::uint16_t kX86 = 0x014C;
    constexpr std::uint16_t kArm64 = 0xAA64;
    if (machine != kX64 && machine != kX86 && machine != kArm64) {
        return makeError(ErrorCode::Unsupported,
                         fmt::format("the executable is built for another processor (machine 0x{:04X})", machine),
                         "executable: " + shown);
    }
    return ok();
}

/// Schemes never activated, whatever list allows them (checkActivationUrl()); "ms-" and "microsoft-" prefixes too.
constexpr std::array<std::string_view, 17> kRefusedSchemes{
    "about", "data",   "file",   "ftp",       "hcp",   "http",  "https",  "javascript", "ldap",
    "mailto", "mk",    "res",    "search",    "search-ms", "shell", "telnet", "vbscript"};

/// An activation names nothing else.
Status checkActivation(const LaunchRequest& request) {
    if (auto valid = checkActivationUrl(request.activationUrl); !valid) {
        return valid;
    }
    const auto field = [](std::string_view name) {
        return makeError(ErrorCode::ValidationFailed, "a protocol activation names no program, arguments or folders",
                         std::string(name));
    };
    if (!request.executable.empty()) {
        return field("executable");
    }
    if (!request.arguments.empty()) {
        return field("arguments");
    }
    if (!request.workingDirectory.empty()) {
        return field("working directory");
    }
    if (!request.environment.empty()) {
        return field("environment");
    }
    if (!request.outputLog.empty()) {
        return field("output log");
    }
    return ok();
}

} // namespace

std::optional<std::string> activationScheme(std::string_view url) {
    const std::size_t colon = url.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    const std::string_view scheme = url.substr(0, colon);
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!letter(scheme.front())) {
        return std::nullopt;
    }
    for (const char c : scheme) {
        if (!letter(c) && !(c >= '0' && c <= '9') && c != '+' && c != '-' && c != '.') {
            return std::nullopt;
        }
    }
    return core::toLowerAscii(scheme);
}

Status checkActivationUrl(std::string_view url) {
    const auto refuse = [](std::string message) {
        return makeError(ErrorCode::ValidationFailed, std::move(message), "activation URL");
    };
    if (url.empty() || url.size() > kMaxActivationUrlLength) {
        return refuse(fmt::format("an activation URL holds 1 to {} bytes", kMaxActivationUrlLength));
    }
    if (!core::isValidUtf8(url)) {
        return refuse("the activation URL is not valid UTF-8 text");
    }
    for (const char c : url) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F || c == ' ' || c == '"') {
            return refuse("the activation URL holds a control character, a blank or a double quote");
        }
    }
    const std::optional<std::string> scheme = activationScheme(url);
    if (!scheme || url.size() == scheme->size() + 1) {
        return refuse("the activation URL has no scheme, or nothing after it");
    }
    const bool refused = std::find(kRefusedSchemes.begin(), kRefusedSchemes.end(), *scheme) != kRefusedSchemes.end() ||
                         scheme->starts_with("ms-") || scheme->starts_with("microsoft-");
    if (refused) {
        return refuse(fmt::format("the scheme \"{}\" opens documents, folders, pages or searches: never activated",
                                  *scheme));
    }
    return ok();
}

std::string_view toString(WaitStrategy strategy) noexcept {
    switch (strategy) {
    case WaitStrategy::DirectProcess:
        return "DirectProcess";
    case WaitStrategy::ProcessTree:
        return "ProcessTree";
    case WaitStrategy::UserConfirmReturn:
        return "UserConfirmReturn";
    case WaitStrategy::FollowWindow:
        return "FollowWindow";
    }
    return "unknown";
}

std::optional<WaitStrategy> parseWaitStrategy(std::string_view text) noexcept {
    if (core::equalsIgnoreCaseAscii(text, "direct") || text == "DirectProcess") {
        return WaitStrategy::DirectProcess;
    }
    if (core::equalsIgnoreCaseAscii(text, "tree") || text == "ProcessTree") {
        return WaitStrategy::ProcessTree;
    }
    if (core::equalsIgnoreCaseAscii(text, "confirm") || text == "UserConfirmReturn") {
        return WaitStrategy::UserConfirmReturn;
    }
    if (core::equalsIgnoreCaseAscii(text, "follow") || text == "FollowWindow") {
        return WaitStrategy::FollowWindow;
    }
    return std::nullopt;
}

Expected<std::string> commandLineOf(const LaunchRequest& request) {
    if (request.isActivation()) {
        return request.activationUrl;
    }
    const auto program = utf8Path(request.executable, "executable");
    if (!program) {
        return propagate(program.error());
    }
    return buildCommandLine(*program, request.arguments,
                            request.quoteEveryArgument ? ArgumentQuoting::Always : ArgumentQuoting::WhenNeeded);
}

std::filesystem::path workingDirectoryOf(const LaunchRequest& request) {
    if (request.isActivation()) {
        return {};
    }
    return request.workingDirectory.empty() ? request.executable.parent_path() : request.workingDirectory;
}

Status validateLaunchRequest(const LaunchRequest& request) {
    if (request.isActivation()) {
        return checkActivation(request);
    }
    const auto executable = utf8Path(request.executable, "executable");
    if (!executable) {
        return propagate(executable.error());
    }
    if (request.executable.empty() || !request.executable.is_absolute()) {
        return makeError(ErrorCode::ValidationFailed, "the executable must be an absolute path",
                         "executable: " + *executable);
    }
    std::error_code ec;
    if (!std::filesystem::exists(request.executable, ec)) {
        return makeError(ErrorCode::NotFound, "the executable does not exist", "executable: " + *executable);
    }
    if (!std::filesystem::is_regular_file(request.executable, ec)) {
        return makeError(ErrorCode::ValidationFailed, "the executable is not a file", "executable: " + *executable);
    }
    const std::string extension = core::toLowerAscii(core::pathToUtf8(request.executable.extension()));
    if (extension == ".bat" || extension == ".cmd") {
        return makeError(ErrorCode::Unsupported,
                         "batch files run through cmd.exe, which parses arguments its own way (%, ^, &, |, <, >): "
                         "only programs are started directly",
                         "executable: " + *executable);
    }
    if (extension != ".exe" && extension != ".com") {
        return makeError(ErrorCode::Unsupported, "only .exe and .com programs are started (no shortcuts or documents)",
                         "executable: " + *executable);
    }
    if (auto program = checkWindowsProgram(request.executable, *executable); !program) {
        return program;
    }

    if (!request.workingDirectory.empty()) {
        const auto directory = utf8Path(request.workingDirectory, "working directory");
        if (!directory) {
            return propagate(directory.error());
        }
        if (!std::filesystem::is_directory(request.workingDirectory, ec)) {
            return makeError(ErrorCode::NotFound, "the working directory does not exist",
                             "working directory: " + *directory);
        }
    }

    if (auto commandLine = commandLineOf(request); !commandLine) {
        return propagate(commandLine.error());
    }

    for (const EnvironmentOverride& override : request.environment) {
        if (auto valid = validateOverride(override); !valid) {
            return valid;
        }
    }

    if (!request.outputLog.empty()) {
        const auto log = utf8Path(request.outputLog, "output log");
        if (!log) {
            return propagate(log.error());
        }
        if (!request.outputLog.is_absolute()) {
            return makeError(ErrorCode::ValidationFailed, "the output log must be an absolute path",
                             "output log: " + *log);
        }
    }
    return ok();
}

WaitStrategy effectiveStrategy(WaitStrategy requested, bool treeFollowed) noexcept {
    if (requested == WaitStrategy::ProcessTree && !treeFollowed) {
        return WaitStrategy::UserConfirmReturn;
    }
    return requested;
}

} // namespace rv::launcher
