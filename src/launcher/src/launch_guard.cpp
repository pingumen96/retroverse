#include <rv/launcher/launch_guard.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#include <rv/core/case_fold.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/system_probe.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>
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
constexpr std::string_view kAppendConfigOption = "--appendconfig";
constexpr std::string_view kAppendConfigGlued = "--appendconfig=";

/// One of RetroArch's long options and whether it takes a value.
struct RetroArchOption {
    std::string_view name;
    bool takesValue;
};

/// RetroArch 1.21.0's long options (retroarch.c, opts[]), those of every build option included: an option that a build
/// leaves out stops that build as an unknown one, which the reading below never relies on.
constexpr std::array<RetroArchOption, 44> kRetroArchLongOptions{{
    {"libretro", true},       {"menu", false},          {"help", false},
    {"save", true},           {"fullscreen", false},    {"record", true},
    {"recordconfig", true},   {"size", true},           {"verbose", false},
    {"config", true},         {"appendconfig", true},   {"nodevice", true},
    {"dualanalog", true},     {"device", true},         {"savestate", true},
    {"set-shader", true},     {"play-replay", true},    {"record-replay", true},
    {"sram-mode", true},      {"host", false},          {"connect", true},
    {"mitm-session", true},   {"check-frames", true},   {"port", true},
    {"command", true},        {"nick", true},           {"ups", true},
    {"bps", true},            {"ips", true},            {"xdelta", true},
    {"no-patch", false},      {"detach", false},        {"features", false},
    {"subsystem", true},      {"max-frames", true},     {"max-frames-ss", false},
    {"max-frames-ss-path", true}, {"eof-exit", false},  {"version", false},
    {"log-file", true},       {"accessibility", false}, {"load-menu-on-error", false},
    {"entryslot", true},      {"scan", true},
}};
// An entry left empty would make "--" a known option.
static_assert(std::none_of(kRetroArchLongOptions.begin(), kRetroArchLongOptions.end(),
                           [](const RetroArchOption& option) { return option.name.empty(); }));

/// RetroArch 1.21.0's short options (retroarch.c, its optstring with every build option): a letter followed by ':'
/// takes a value. The letters of long options missing here ('T' of --mitm-session) are no short options.
constexpr std::string_view kRetroArchShortOptions = "hs:fvVS:A:U:DN:d:e:P:R:M:HC:F:L:r:c:";

/// RetroArch keeps --appendconfig's value in a buffer of PATH_MAX_LENGTH (4096) bytes, its terminator included: the
/// rest of a longer value is cut off.
constexpr std::size_t kRetroArchMaxAppendConfig = 4095;

/// RetroArch is not long-path aware (its manifest declares nothing but DPI awareness): a plain path of MAX_PATH (260)
/// UTF-16 code units or more does not open.
constexpr std::size_t kRetroArchMaxPathUnits = 259;

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

/// Which file a handle opens: its volume and its id there. Two names are one file only when these match: a folder
/// with case-sensitive names (NTFS, per folder) holds "emu.exe" and "EMU.EXE" as two files whose paths fold alike.
struct FileIdentity {
    std::uint64_t volume = 0;
    std::array<unsigned char, 16> id{};
    /// A 128-bit id (FILE_ID_INFO); otherwise the 64-bit index of the older query, in the first 8 bytes.
    bool full = false;

    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

/// The file a path opens: its path as the file system names it and its identity.
struct ResolvedFile {
    std::wstring path;
    FileIdentity identity;
};

#if defined(_WIN32)
/// The identity of the file `handle` opens: FILE_ID_INFO, else BY_HANDLE_FILE_INFORMATION (file systems without
/// 128-bit ids); nothing when the file system gives neither, the reason in GetLastError().
[[nodiscard]] std::optional<FileIdentity> identityOf(HANDLE handle) {
    FileIdentity identity;
    FILE_ID_INFO info{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        static_assert(sizeof(info.FileId.Identifier) == sizeof(identity.id));
        identity.volume = info.VolumeSerialNumber;
        std::memcpy(identity.id.data(), info.FileId.Identifier, identity.id.size());
        identity.full = true;
        return identity;
    }
    BY_HANDLE_FILE_INFORMATION legacy{};
    if (GetFileInformationByHandle(handle, &legacy)) {
        identity.volume = legacy.dwVolumeSerialNumber;
        const std::uint64_t index = (static_cast<std::uint64_t>(legacy.nFileIndexHigh) << 32) | legacy.nFileIndexLow;
        std::memcpy(identity.id.data(), &index, sizeof(index));
        return identity;
    }
    return std::nullopt;
}
#endif

/// The file `path` opens, as the file system names it (8.3 short names expanded, symbolic links and junctions followed,
/// subst and mapped drives resolved: GetFinalPathNameByHandleW, without the \\?\ prefix), and which file it is. Nothing
/// when it cannot be opened or told apart, the reason in `error`. Opened for its attributes only.
[[nodiscard]] std::optional<ResolvedFile> resolvedFile(const std::filesystem::path& path, std::string* error) {
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
    DWORD failure = ERROR_SUCCESS;
    std::optional<FileIdentity> identity;
    if (length == 0 || length >= resolved.size()) {
        failure = GetLastError();
    } else {
        identity = identityOf(handle);
        if (!identity) {
            failure = GetLastError();
        }
    }
    CloseHandle(handle);
    if (failure != ERROR_SUCCESS || !identity) {
        if (error != nullptr) {
            *error = core::systemErrorMessage(failure);
        }
        return std::nullopt;
    }
    resolved.resize(length);
    return ResolvedFile{withoutPrefix(std::move(resolved)), *identity};
#else
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    if (ec) {
        if (error != nullptr) {
            *error = ec.message();
        }
        return std::nullopt;
    }
    return ResolvedFile{canonical.wstring(), {}};
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

[[nodiscard]] bool isAscii(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

/// ` ("<text>")` for a message when `text` is short printable ASCII (an option as written), else nothing: arguments
/// may hold the user's paths, which messages never show.
[[nodiscard]] std::string shown(std::string_view text) {
    constexpr std::size_t kMaxShown = 40;
    const bool printable = std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c < 0x7F; });
    return printable && !text.empty() && text.size() <= kMaxShown ? fmt::format(" (\"{}\")", text) : std::string();
}

/// True when the ANSI code page, in which RetroArch reads its command line, is UTF-8 (the system-wide option of
/// Windows): every character is held then.
[[nodiscard]] bool ansiCodePageIsUtf8() noexcept {
#if defined(_WIN32)
    return GetACP() == CP_UTF8;
#else
    return true;
#endif
}

#if defined(_WIN32)
/// True when `utf8` comes back unchanged from the ANSI code page `codePage`, converted as Windows converts a command line
/// for a program that reads it through main() (best-fit characters allowed: a fullwidth quotation mark becomes '"').
[[nodiscard]] bool survivesCodePage(std::string_view utf8, UINT codePage) {
    const auto wide = core::utf8ToWide(utf8);
    if (!wide || wide->size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (wide->empty()) {
        return true;
    }
    const int units = static_cast<int>(wide->size());
    const int bytes = WideCharToMultiByte(codePage, 0, wide->data(), units, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return false;
    }
    std::string ansi(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(codePage, 0, wide->data(), units, ansi.data(), bytes, nullptr, nullptr) != bytes) {
        return false;
    }
    const int back = MultiByteToWideChar(codePage, 0, ansi.data(), bytes, nullptr, 0);
    if (back <= 0) {
        return false;
    }
    std::wstring returned(static_cast<std::size_t>(back), L'\0');
    if (MultiByteToWideChar(codePage, 0, ansi.data(), bytes, returned.data(), back) != back) {
        return false;
    }
    return returned == *wide;
}
#endif

/// Why RetroArch would not read `request`'s command line as it is written, or nothing. Its main() reads the ANSI code
/// page's form of the command line (its manifest declares no UTF-8 code page), which Windows makes with best-fit
/// characters: text that the code page does not hold exactly reaches RetroArch as other text (a fullwidth quotation
/// mark splits an argument in two, a fullwidth hyphen starts an option). The guard's own process has the same code page:
/// Retroverse's manifests declare none either.
[[nodiscard]] std::optional<std::string> codePageProblem(const LaunchRequest& request) {
#if defined(_WIN32)
    const UINT codePage = GetACP();
    if (codePage == CP_UTF8) {
        return std::nullopt;
    }
    const auto executable = core::tryPathToUtf8(request.executable);
    if (!executable || !survivesCodePage(*executable, codePage)) {
        return fmt::format("the executable's path is not held exactly by the ANSI code page {}, in which RetroArch "
                           "reads its command line",
                           codePage);
    }
    for (std::size_t i = 0; i < request.arguments.size(); ++i) {
        if (!survivesCodePage(request.arguments[i], codePage)) {
            return fmt::format("arguments[{}] is not held exactly by the ANSI code page {}, in which RetroArch reads "
                               "its command line",
                               i, codePage);
        }
    }
#else
    (void)request;
#endif
    return std::nullopt;
}

/// Why RetroArch would not append every file of an --appendconfig value ('|'-separated), or nothing. RetroArch only logs
/// a file it cannot open and runs on its own configuration, so every file must be one it opens: it keeps 4095 bytes of
/// the value, opens the files named by its ANSI arguments as UTF-8 text and is not long-path aware. Each file is an
/// absolute drive or UNC path (no \\?\ form, device or stream), in ASCII unless the ANSI code page is UTF-8, of fewer
/// than 260 UTF-16 code units as written and once normalized, and an existing file.
[[nodiscard]] std::optional<std::string> appendConfigProblem(std::string_view value) {
    if (value.empty()) {
        return "it names no file";
    }
    if (value.size() > kRetroArchMaxAppendConfig) {
        return fmt::format("its value is longer than the {} bytes RetroArch keeps of it", kRetroArchMaxAppendConfig);
    }
    for (const std::string_view part : core::split(value, '|')) {
        auto path = core::tryPathFromUtf8(part);
        if (part.empty() || !path || unsupportedForm(*path) || path->native().starts_with(L"\\\\?\\")) {
            return "a file it names is not an absolute drive or UNC path";
        }
        if (!isAscii(part) && !ansiCodePageIsUtf8()) {
            return "a file it names is not in ASCII, and RetroArch opens the files of its ANSI arguments as UTF-8 "
                   "text";
        }
        if (utf16Length(part) > kRetroArchMaxPathUnits || normalizedForm(*path).size() > kRetroArchMaxPathUnits) {
            return fmt::format("a file it names has a path longer than {} characters, which RetroArch (not long-path "
                               "aware) cannot open",
                               kRetroArchMaxPathUnits);
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::path(core::extendedLengthPath(*path)), ec)) {
            return "a file it names does not exist";
        }
    }
    return std::nullopt;
}

/// The name RetroArch reads in the long option "--<body>": the first run of characters other than '=' (strtok_r), so
/// that "--=appendconfig" is --appendconfig.
[[nodiscard]] std::string_view readLongName(std::string_view body) noexcept {
    const std::size_t start = body.find_first_not_of('=');
    if (start == std::string_view::npos) {
        return {};
    }
    const std::size_t end = body.find('=', start);
    return body.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
}

/// True when a long option's name may select --appendconfig in some reading of it: that name, an abbreviation of it
/// (GNU getopt_long accepts unique abbreviations) or a name holding it.
[[nodiscard]] bool mayNameAppendConfig(std::string_view name) noexcept {
    return !name.empty() && (kAppendConfig.starts_with(name) || name.find(kAppendConfig) != std::string_view::npos);
}

/// The option of exactly this name (RetroArch accepts no abbreviation); nothing for an unknown or empty name.
[[nodiscard]] const RetroArchOption* longOption(std::string_view name) {
    if (name.empty()) {
        return nullptr;
    }
    const auto found = std::find_if(kRetroArchLongOptions.begin(), kRetroArchLongOptions.end(),
                                    [&](const RetroArchOption& option) { return option.name == name; });
    return found == kRetroArchLongOptions.end() ? nullptr : &*found;
}

/// Why RetroArch's arguments may leave it on its own configuration, or nothing when they surely append usable
/// configuration files. RetroArch 1.21.0's Windows builds read them with libretro-common's compat getopt: every option
/// wherever it stands (the other arguments are moved behind the options); long options by their exact names, the name
/// being the first run of characters other than '=' after "--" (no abbreviations; "--" itself is no option); a long
/// option's value after the first '=' of its argument, else the next argument; short options from its option string,
/// grouped, a value the rest of the argument or the next one; an unknown option or a lone "-" stops RetroArch before
/// it reads any configuration; --appendconfig read again replaces the earlier value. The checks, the first failure
/// named:
///  1. the command line reaches RetroArch as written (codePageProblem());
///  2. every argument that may select --appendconfig in any reading (mayNameAppendConfig() of its name, read either
///     way), wherever it stands, is "--appendconfig=<files>" or "--appendconfig" and the next argument, with usable
///     files (appendConfigProblem());
///  3. the arguments read as RetroArch reads them hold no option it does not know, no lone "-", no grouped short
///     options running into a long one ("-v-appendconfig"), no option without its value (RetroArch would take it from
///     the arguments moved behind: refused rather than modelled), and at least one --appendconfig, each with usable
///     files.
[[nodiscard]] std::optional<std::string> retroArchIsolationProblem(const LaunchRequest& request) {
    if (auto problem = codePageProblem(request)) {
        return problem;
    }
    const std::span<const std::string> arguments = request.arguments;

    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (!argument.starts_with("--")) {
            continue;
        }
        const std::string_view body = argument.substr(2);
        if (!mayNameAppendConfig(body.substr(0, body.find('='))) && !mayNameAppendConfig(readLongName(body))) {
            continue;
        }
        std::optional<std::string_view> value;
        if (argument == kAppendConfigOption) {
            if (i + 1 < arguments.size()) {
                value = arguments[i + 1];
            }
        } else if (argument.starts_with(kAppendConfigGlued)) {
            value = argument.substr(kAppendConfigGlued.size());
        } else {
            return fmt::format("arguments[{}] may be read as --appendconfig but is not \"--appendconfig\" or "
                               "\"--appendconfig=<files>\"",
                               i);
        }
        if (!value) {
            return fmt::format("arguments[{}] (--appendconfig) has no value after it", i);
        }
        if (auto problem = appendConfigProblem(*value)) {
            return fmt::format("arguments[{}] (--appendconfig): {}", i, *problem);
        }
    }

    bool appended = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view argument = arguments[i];
        if (argument.empty() || argument.front() != '-') {
            continue; // not an option: RetroArch moves it behind the options
        }
        const std::size_t index = i;
        if (argument.starts_with("--")) {
            const std::string_view name = readLongName(argument.substr(2));
            const RetroArchOption* option = longOption(name);
            if (option == nullptr) {
                return fmt::format("arguments[{}] is an option RetroArch does not know{}, which stops it", index,
                                   shown(fmt::format("--{}", name)));
            }
            if (!option->takesValue) {
                continue; // a value glued to an option without one is ignored
            }
            std::string_view value;
            if (const std::size_t equals = argument.find('='); equals != std::string_view::npos) {
                value = argument.substr(equals + 1);
            } else if (i + 1 < arguments.size()) {
                value = arguments[++i];
            } else {
                return fmt::format("arguments[{}] (--{}) has no value after it", index, option->name);
            }
            if (option->name == kAppendConfig) {
                if (auto problem = appendConfigProblem(value)) {
                    return fmt::format("arguments[{}] is read as --appendconfig: {}", index, *problem);
                }
                appended = true;
            }
            continue;
        }
        if (argument.size() == 1) {
            return fmt::format("arguments[{}] is a lone \"-\", which stops RetroArch", index);
        }
        // Grouped short options: flags one after the other; the first that takes a value takes the rest of the
        // argument, else the next argument.
        for (std::size_t k = 1; k < argument.size(); ++k) {
            const char letter = argument[k];
            if (letter == '-') {
                return fmt::format("arguments[{}] runs short options into a long one", index);
            }
            const std::size_t at = letter == ':' ? std::string_view::npos : kRetroArchShortOptions.find(letter);
            if (at == std::string_view::npos) {
                return fmt::format("arguments[{}] holds an option RetroArch does not know{}, which stops it", index,
                                   shown(std::string{'-', letter}));
            }
            if (at + 1 < kRetroArchShortOptions.size() && kRetroArchShortOptions[at + 1] == ':') {
                if (k + 1 == argument.size()) {
                    if (i + 1 == arguments.size()) {
                        return fmt::format("arguments[{}] (-{}) has no value after it", index, letter);
                    }
                    ++i;
                }
                break;
            }
        }
    }
    if (!appended) {
        return "no argument is read as --appendconfig";
    }
    return std::nullopt;
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
    case GuardRule::Activation:
        return "activation";
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
    if (request.isActivation()) {
        return GuardVerdict{GuardRule::Activation,
                            fmt::format("a protocol activation never runs in an automated run ({} names programs, "
                                        "not schemes): {}",
                                        kLaunchAllowListVariable, request.activationUrl)};
    }
    const std::string executable = core::pathToUtf8Lossy(request.executable);
    const auto refuse = [&](GuardRule rule, std::string_view why) {
        return GuardVerdict{rule, fmt::format("{}: {}", why, executable)};
    };
    if (const auto form = unsupportedForm(request.executable)) {
        return refuse(GuardRule::UnsupportedPath,
                      fmt::format("only absolute executable paths are checked against {}, not {}",
                                  kLaunchAllowListVariable, *form));
    }

    // Both names count: the one Windows opens (a link's own name, an 8.3 name) and the file it resolves to.
    const std::wstring normalized = normalizedForm(request.executable);
    std::string openError;
    const std::optional<ResolvedFile> resolved = resolvedFile(request.executable, &openError);
    const std::string requestedName = fileName(normalized);
    const std::string resolvedName = resolved ? fileName(resolved->path) : std::string();
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
    if (named(kRetroArchPattern)) {
        if (const auto problem = retroArchIsolationProblem(request)) {
            return refuse(GuardRule::RetroArchNotIsolated,
                          fmt::format("RetroArch starts in an automated run only isolated by --appendconfig naming "
                                      "configuration files it opens ({})",
                                      *problem));
        }
    }

    // The same final path (folded) and the same file: an entry that cannot be opened allows nothing.
    const std::wstring key = folded(resolved->path);
    for (const std::filesystem::path& entry : m_entries) {
        const std::optional<ResolvedFile> listed = resolvedFile(entry, nullptr);
        if (listed && listed->identity == resolved->identity && folded(listed->path) == key) {
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

bool isGuardRefusal(const Error& error) noexcept {
    return error.code == ErrorCode::ValidationFailed && error.context == kLaunchAllowListVariable;
}

bool launchTestAllowed(bool frameLimited) {
    const auto background = processEnvironmentVariable("RV_LAUNCH_BACKGROUND");
    return launchTestAllowed(frameLimited, background && !background->empty() && *background != "0",
                             processEnvironmentVariable(kLaunchAllowListVariable).has_value());
}

} // namespace rv::launcher
