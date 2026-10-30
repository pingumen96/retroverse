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

/// A reserved DOS device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9, and COM and LPT with a superscript ¹, ² or ³:
/// Microsoft's list of names never to give a file, COM0 and LPT0 included),
/// ignoring ASCII case: Windows opens the device for an element whose stem (up to its first dot) is one, whatever its
/// extension ("nul.pdf" is NUL). Trailing spaces of the stem are refused elsewhere (an element never ends with one, but
/// "CON .pdf" is CON too): they are dropped here.
[[nodiscard]] bool isReservedDeviceName(std::string_view element) noexcept {
    std::string_view stem = element.substr(0, std::min(element.find('.'), element.size()));
    while (!stem.empty() && stem.back() == ' ') {
        stem.remove_suffix(1);
    }
    for (const std::string_view name : {"CON", "PRN", "AUX", "NUL"}) {
        if (equalsIgnoringAsciiCase(stem, name)) {
            return true;
        }
    }
    if (stem.size() < 4) {
        return false;
    }
    const std::string_view prefix = stem.substr(0, 3);
    if (!equalsIgnoringAsciiCase(prefix, "COM") && !equalsIgnoringAsciiCase(prefix, "LPT")) {
        return false;
    }
    const std::string_view digit = stem.substr(3);
    if (digit.size() == 1) {
        return digit[0] >= '0' && digit[0] <= '9';
    }
    // U+00B9, U+00B2, U+00B3 in UTF-8.
    return digit == "\xC2\xB9" || digit == "\xC2\xB2" || digit == "\xC2\xB3";
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
    // A colon past the drive's names an alternate data stream (or a device: "COM1:").
    if (path.find(':', isDrivePath(path) ? 2 : 0) != std::string::npos) {
        return unsupported("the manual's path holds a colon past its drive");
    }
    // An element Windows would change when it normalizes the plain path (a trailing dot or space) names another file;
    // one whose stem is a reserved device name names a device.
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
        if (isReservedDeviceName(std::string_view(path).substr(start, end - start))) {
            return unsupported("an element of the manual's path is a reserved device name");
        }
        start = end + 1;
    }
    // explorer.exe splits an argument at its commas (its own switches' syntax). The plan always quotes the argument
    // (quoteEveryArgument), but whether explorer.exe keeps a quoted comma cannot be proved without opening a window: a
    // comma is refused anyway.
    if (path.find(',') != std::string::npos) {
        return unsupported("the manual's path holds a comma, which explorer.exe may split");
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
    // In double quotes whatever it holds: explorer.exe reads an unquoted argument its own way.
    request.quoteEveryArgument = true;
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
