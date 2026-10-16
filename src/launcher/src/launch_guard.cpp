#include <rv/launcher/launch_guard.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/environment.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace rv::launcher {
namespace {

constexpr std::string_view kDuckStationPattern = "duckstation*.exe";
constexpr std::string_view kRetroArchPattern = "retroarch*.exe";
constexpr std::string_view kAppendConfig = "appendconfig";

/// One of RetroArch's command-line options (retroarch.c): its long name, its letter (0: none), whether it takes a
/// value.
struct RetroArchOption {
    std::string_view name;
    char letter;
    bool takesValue;
};

/// RetroArch's options, read the way its getopt_long reads them so that a value is never taken for an option ("-L
/// --appendconfig" passes "--appendconfig" as the core). An option it does not know stops RetroArch with an error.
constexpr std::array<RetroArchOption, 43> kRetroArchOptions{{
    {"libretro", 'L', true},     {"menu", 0, false},           {"help", 'h', false},
    {"save", 's', true},         {"fullscreen", 'f', false},   {"record", 'r', true},
    {"recordconfig", 0, true},   {"size", 0, true},            {"verbose", 'v', false},
    {"config", 'c', true},       {"appendconfig", 0, true},    {"nodevice", 'N', true},
    {"dualanalog", 'A', true},   {"device", 'd', true},        {"savestate", 'S', true},
    {"set-shader", 0, true},     {"bsvplay", 'P', true},       {"bsvrecord", 'R', true},
    {"sram-mode", 'M', true},    {"host", 'H', false},         {"connect", 'C', true},
    {"stateless", 0, false},     {"check-frames", 0, true},    {"port", 0, true},
    {"command", 0, true},        {"nick", 0, true},            {"ups", 'U', true},
    {"bps", 0, true},            {"ips", 0, true},             {"xdelta", 0, true},
    {"no-patch", 0, false},      {"detach", 'D', false},       {"features", 0, false},
    {"subsystem", 0, true},      {"max-frames", 0, true},      {"max-frames-ss", 0, false},
    {"max-frames-ss-path", 0, true}, {"eof-exit", 0, false},   {"version", 0, false},
    {"log-file", 0, true},       {"accessibility", 0, false},  {"load-menu-on-error", 0, false},
    {"entryslot", 'e', true},
}};

/// `text` without its \\?\ prefix: "\\?\C:\x" is "C:\x", "\\?\UNC\server\share\x" is "\\server\share\x".
[[nodiscard]] std::wstring withoutPrefix(std::wstring text) {
    if (text.starts_with(L"\\\\?\\UNC\\")) {
        return L"\\\\" + text.substr(8);
    }
    if (text.starts_with(L"\\\\?\\")) {
        return text.substr(4);
    }
    return text;
}

[[nodiscard]] bool isDriveRoot(std::wstring_view text) noexcept {
    return text.size() >= 3 && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) &&
           text[1] == L':' && (text[2] == L'\\' || text[2] == L'/');
}

/// Why the guard cannot compare `path`, or nothing when it can: a drive or UNC path, absolute, also in its \\?\ form;
/// never a relative path, a device namespace (\\.\, \\?\GLOBALROOT, \\?\Volume{...}, \??\) or a stream.
[[nodiscard]] std::optional<std::string> unsupportedForm(const std::filesystem::path& path) {
    const std::wstring& text = path.native();
    if (text.empty()) {
        return "an empty path";
    }
    if (text.starts_with(L"\\\\.\\") || text.starts_with(L"//./") || text.starts_with(L"//?/") ||
        text.starts_with(L"\\??\\")) {
        return "a device path";
    }
    if (text.starts_with(L"\\\\?\\")) {
        const std::wstring_view rest = std::wstring_view(text).substr(4);
        if (!isDriveRoot(rest) && !rest.starts_with(L"UNC\\")) {
            return "a device path";
        }
    } else if (!path.is_absolute()) {
        return "a relative path";
    }
    const std::wstring plain = withoutPrefix(text);
    if (plain.find(L':', isDriveRoot(plain) ? 2 : 0) != std::wstring::npos) {
        return "an alternate data stream";
    }
    return std::nullopt;
}

/// The path as Windows normalizes it before it opens it (absolute, "." and ".." resolved, '/' turned into '\', trailing
/// dots and spaces dropped from the elements), without the \\?\ prefix.
[[nodiscard]] std::wstring normalizedForm(const std::filesystem::path& path) {
    return withoutPrefix(core::extendedLengthPath(path));
}

/// The path of the file `path` opens, as the file system names it: 8.3 short names expanded, symbolic links and
/// junctions followed, subst and mapped drives resolved (GetFinalPathNameByHandleW), without the \\?\ prefix. Nothing
/// when it cannot be opened, the reason in `error`. Opened for its attributes only.
[[nodiscard]] std::optional<std::wstring> resolvedForm(const std::filesystem::path& path, std::string* error) {
#if defined(_WIN32)
    const std::wstring target = core::extendedLengthPath(path);
    const HANDLE handle =
        CreateFileW(target.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = core::lastSystemErrorMessage();
        }
        return std::nullopt;
    }
    std::wstring resolved(MAX_PATH, L'\0');
    DWORD length = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        length = GetFinalPathNameByHandleW(handle, resolved.data(), static_cast<DWORD>(resolved.size()),
                                           FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0 || length < resolved.size()) {
            break;
        }
        resolved.resize(length + 1); // the size it needs, its terminator included
    }
    const DWORD failure = GetLastError();
    CloseHandle(handle);
    if (length == 0 || length >= resolved.size()) {
        if (error != nullptr) {
            *error = core::systemErrorMessage(failure);
        }
        return std::nullopt;
    }
    resolved.resize(length);
    return withoutPrefix(std::move(resolved));
#else
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    if (ec) {
        if (error != nullptr) {
            *error = ec.message();
        }
        return std::nullopt;
    }
    return canonical.wstring();
#endif
}

/// `form` upper-cased as the file system compares names (as it is should the casing fail: compared exactly).
[[nodiscard]] std::wstring folded(const std::wstring& form) {
    auto upper = core::fileSystemUpperCase(std::wstring_view(form));
    return upper ? std::move(*upper) : form;
}

[[nodiscard]] std::string fileName(const std::wstring& form) {
    return core::wideToUtf8Lossy(std::filesystem::path(form).filename().native());
}

/// Every '|'-separated file of an --appendconfig value is an absolute path to an existing file.
[[nodiscard]] bool usableAppendConfig(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const std::string_view part : core::split(value, '|')) {
        auto path = core::tryPathFromUtf8(part);
        if (part.empty() || !path || unsupportedForm(*path)) {
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::path(core::extendedLengthPath(*path)), ec)) {
            return false;
        }
    }
    return true;
}

/// The option a long name selects: its exact name, else the one option it abbreviates (getopt_long accepts unique
/// abbreviations); nothing for a name RetroArch does not know or that abbreviates several options.
[[nodiscard]] const RetroArchOption* longOption(std::string_view name) {
    const RetroArchOption* abbreviated = nullptr;
    for (const RetroArchOption& option : kRetroArchOptions) {
        if (option.name == name) {
            return &option;
        }
        if (!name.empty() && option.name.starts_with(name)) {
            if (abbreviated != nullptr) {
                return nullptr; // ambiguous
            }
            abbreviated = &option;
        }
    }
    return abbreviated;
}

[[nodiscard]] const RetroArchOption* shortOption(char letter) {
    const auto found = std::find_if(kRetroArchOptions.begin(), kRetroArchOptions.end(),
                                    [&](const RetroArchOption& option) { return option.letter == letter; });
    return found == kRetroArchOptions.end() ? nullptr : &*found;
}

/// True when RetroArch's arguments surely append existing configuration files, in two passes:
///  1. every element that spells --appendconfig or abbreviates it ("--appendc"), wherever it stands, is the option
///     spelled out with a usable value ("--appendconfig=<files>", or "--appendconfig" and the next element): whichever
///     of them RetroArch takes last, it isolates; one with another spelling, an abbreviation (which getopt_long may
///     accept) or no value refuses;
///  2. one of them is read as the option, the arguments read as RetroArch's getopt_long reads them: options before the
///     first "--" (non-options are permuted), the value of an option that takes one skipped (the next element unless
///     glued), grouped short options. An option RetroArch does not know, or an ambiguous abbreviation, stops RetroArch
///     with an error before it runs anything: refused as well, since the reading is no longer sure.
[[nodiscard]] bool isolatedRetroArch(std::span<const std::string> arguments) {
    const auto split = [](std::string_view argument) {
        const std::string_view body = argument.substr(2);
        const std::size_t equals = body.find('=');
        return std::pair{body.substr(0, equals),
                         equals == std::string_view::npos ? std::optional<std::string_view>()
                                                          : std::optional<std::string_view>(body.substr(equals + 1))};
    };
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (!argument.starts_with("--")) {
            continue;
        }
        const auto [name, glued] = split(argument);
        if (name.size() < 2 || !kAppendConfig.starts_with(name)) {
            continue;
        }
        std::optional<std::string_view> value = glued;
        if (!value && i + 1 < arguments.size()) {
            value = arguments[i + 1];
        }
        if (name != kAppendConfig || !value || !usableAppendConfig(*value)) {
            return false;
        }
    }
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (argument == "--") {
            return false; // what follows is content, never an option
        }
        if (argument.starts_with("--")) {
            const auto [name, glued] = split(argument);
            const RetroArchOption* option = longOption(name);
            if (option == nullptr) {
                return false;
            }
            if (option->name == kAppendConfig) {
                return name == kAppendConfig; // its value is usable (first pass)
            }
            if (option->takesValue && !glued) {
                ++i;
            }
            continue;
        }
        if (argument.size() > 1 && argument.front() == '-') {
            // Grouped short options: the first one that takes a value takes the rest of the element, else the next one.
            for (std::size_t k = 1; k < argument.size(); ++k) {
                const RetroArchOption* option = shortOption(argument[k]);
                if (option == nullptr) {
                    return false;
                }
                if (option->takesValue) {
                    if (k + 1 == argument.size()) {
                        ++i;
                    }
                    break;
                }
            }
        }
    }
    return false;
}

} // namespace

std::string_view toString(GuardRule rule) noexcept {
    switch (rule) {
    case GuardRule::Inactive:
        return "inactive";
    case GuardRule::Allowed:
        return "allowed";
    case GuardRule::UnsupportedPath:
        return "unsupported path";
    case GuardRule::DuckStation:
        return "DuckStation";
    case GuardRule::Unresolved:
        return "unresolved";
    case GuardRule::RetroArchNotIsolated:
        return "RetroArch not isolated";
    case GuardRule::NotListed:
        return "not listed";
    }
    return "unknown";
}

LaunchGuard LaunchGuard::fromAllowList(std::string_view allowList) {
    LaunchGuard guard;
    guard.m_active = true;
    for (std::string_view entry : core::split(allowList, ';')) {
        entry = core::trim(entry);
        if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
            entry = core::trim(entry.substr(1, entry.size() - 2));
        }
        if (entry.empty()) {
            continue;
        }
        auto path = core::tryPathFromUtf8(entry);
        if (!path || unsupportedForm(*path)) {
            guard.m_ignored.emplace_back(entry);
            continue;
        }
        guard.m_entries.push_back(std::move(*path));
    }
    return guard;
}

LaunchGuard LaunchGuard::fromEnvironment() {
    const auto value = processEnvironmentVariable(kLaunchAllowListVariable);
    return value ? fromAllowList(*value) : LaunchGuard{};
}

GuardVerdict LaunchGuard::check(const LaunchRequest& request) const {
    if (!m_active) {
        return GuardVerdict{GuardRule::Inactive, {}};
    }
    const std::string shown = core::pathToUtf8Lossy(request.executable);
    const auto refuse = [&](GuardRule rule, std::string_view why) {
        return GuardVerdict{rule, fmt::format("{}: {}", why, shown)};
    };
    if (const auto form = unsupportedForm(request.executable)) {
        return refuse(GuardRule::UnsupportedPath,
                      fmt::format("only absolute executable paths are checked against {}, not {}",
                                  kLaunchAllowListVariable, *form));
    }

    // Both names count: the one Windows opens (a link's own name, an 8.3 name) and the file it resolves to.
    const std::wstring normalized = normalizedForm(request.executable);
    std::string openError;
    const std::optional<std::wstring> resolved = resolvedForm(request.executable, &openError);
    const std::string requestedName = fileName(normalized);
    const std::string resolvedName = resolved ? fileName(*resolved) : std::string();
    const auto named = [&](std::string_view pattern) {
        return core::matchesWildcard(requestedName, pattern) ||
               (resolved && core::matchesWildcard(resolvedName, pattern));
    };
    if (named(kDuckStationPattern)) {
        return refuse(GuardRule::DuckStation, "DuckStation never starts in an automated run (its settings, memory "
                                              "cards and caches cannot be isolated)");
    }
    if (!resolved) {
        return refuse(GuardRule::Unresolved,
                      fmt::format("the executable cannot be opened to see which file it is ({})", openError));
    }
    if (named(kRetroArchPattern) && !isolatedRetroArch(request.arguments)) {
        return refuse(GuardRule::RetroArchNotIsolated,
                      "RetroArch starts in an automated run only isolated: --appendconfig naming existing "
                      "configuration files by absolute paths");
    }

    const std::wstring key = folded(*resolved);
    for (const std::filesystem::path& entry : m_entries) {
        const std::optional<std::wstring> entryResolved = resolvedForm(entry, nullptr);
        if (folded(entryResolved ? *entryResolved : normalizedForm(entry)) == key) {
            return GuardVerdict{GuardRule::Allowed, {}};
        }
    }
    return refuse(GuardRule::NotListed, fmt::format("the executable is not listed in {}", kLaunchAllowListVariable));
}

Status checkLaunchGuard(const LaunchRequest& request) {
    const GuardVerdict verdict = LaunchGuard::fromEnvironment().check(request);
    if (verdict.allowed()) {
        return ok();
    }
    return makeError(ErrorCode::ValidationFailed, "refused by the automation guard: " + verdict.message,
                     std::string(kLaunchAllowListVariable));
}

} // namespace rv::launcher
