#include <rv/launcher/manual_open.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

#include <fmt/format.h>

#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>

namespace rv::launcher {
namespace {

[[nodiscard]] tl::unexpected<Error> unsupported(std::string message) {
    return makeError(ErrorCode::Unsupported, std::move(message), "manual");
}

[[nodiscard]] bool equalsIgnoringAsciiCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

[[nodiscard]] bool isSeparator(char c) noexcept {
    return c == '\\' || c == '/';
}

/// "C:\..." with a drive letter.
[[nodiscard]] bool isDrivePath(std::string_view path) noexcept {
    return path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':' &&
           isSeparator(path[2]);
}

/// "\\server\share\..." (not "\\?\" or "\\.\").
[[nodiscard]] bool isUncPath(std::string_view path) noexcept {
    return path.size() > 2 && isSeparator(path[0]) && isSeparator(path[1]) && path[2] != '?' && path[2] != '.' &&
           !isSeparator(path[2]);
}

} // namespace

Expected<std::string> manualArgument(const std::filesystem::path& manual) {
    auto utf8 = core::tryPathToUtf8(manual);
    if (!utf8) {
        return makeError(ErrorCode::InvalidArgument, "the manual's path is not UTF-8", "manual");
    }
    std::string path = std::move(*utf8);
    // The \\?\ form the media root's paths take (core::extendedLengthPathBelow) given back as what it stands for.
    if (path.starts_with("\\\\?\\UNC\\") || path.starts_with("\\\\?\\unc\\")) {
        path = "\\\\" + path.substr(8);
    } else if (path.starts_with("\\\\?\\")) {
        path = path.substr(4);
    }
    if (path.starts_with("\\\\.\\") || path.starts_with("\\\\?\\")) {
        return unsupported("a device path is no manual explorer.exe can open");
    }
    if (!isDrivePath(path) && !isUncPath(path)) {
        return unsupported("the manual's path is not absolute");
    }
    const std::size_t dot = path.find_last_of(".\\/");
    if (dot == std::string::npos || path[dot] != '.' ||
        !equalsIgnoringAsciiCase(std::string_view(path).substr(dot), kManualExtension)) {
        return unsupported("the manual is not a .pdf file");
    }
    if (utf16Length(path) > kMaxManualPathUnits) {
        return unsupported(
            fmt::format("the manual's path is longer than the {} characters explorer.exe reads", kMaxManualPathUnits));
    }
    // An element Windows would change when it normalizes the plain path (a trailing dot or space) names another file.
    std::size_t start = isUncPath(path) ? 2 : 3;
    while (start < path.size()) {
        std::size_t end = start;
        while (end < path.size() && !isSeparator(path[end])) {
            ++end;
        }
        if (end == start) {
            return unsupported("the manual's path has an empty element");
        }
        const char last = path[end - 1];
        if (last == '.' || last == ' ') {
            return unsupported("an element of the manual's path ends with a dot or a space");
        }
        start = end + 1;
    }
    // explorer.exe splits an unquoted argument at its commas (its own switches' syntax); appendQuotedArgument() quotes
    // an argument that holds a space or a tab.
    if (path.find(',') != std::string::npos && path.find_first_of(" \t") == std::string::npos) {
        return unsupported("the manual's path holds a comma and no space, which explorer.exe would split");
    }
    return path;
}

Expected<LaunchRequest> planManualOpen(const std::filesystem::path& manual, const core::ISystemProbe& probe,
                                       std::string label) {
    auto argument = manualArgument(manual);
    if (!argument) {
        return propagate(argument.error());
    }
    const std::filesystem::path windows = probe.windowsFolder();
    if (windows.empty() || !windows.is_absolute()) {
        return makeError(ErrorCode::NotFound, "the Windows folder is not known", "explorer.exe");
    }
    LaunchRequest request;
    request.executable = windows / std::filesystem::path(kManualOpenerProgram);
    request.arguments.push_back(std::move(*argument));
    request.workingDirectory = windows;
    request.waitStrategy = WaitStrategy::DirectProcess;
    // It hands the file to the shell and exits at once (with exit code 1 as often as 0): no grace, no window to wait for.
    request.timeouts.startupGrace = std::chrono::milliseconds{0};
    request.timeouts.windowTimeout = std::chrono::milliseconds{0};
    request.label = label.empty() ? std::string("a manual") : std::move(label);
    if (Status valid = validateLaunchRequest(request); !valid) {
        return propagate(valid.error());
    }
    return request;
}

} // namespace rv::launcher
