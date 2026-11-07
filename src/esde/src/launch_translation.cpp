#include <rv/esde/launch_translation.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include <windows.h>

#include <fmt/format.h>

#include <rv/core/file_io.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/command_template.hpp>
#include <rv/launcher/launch_plan.hpp>
#include <rv/launcher/launch_request.hpp>

namespace rv::esde {
namespace {

using catalog::FoundLocation;
using catalog::LaunchReadiness;
using catalog::LaunchTranslation;
using catalog::ReadinessBlocker;

/// Where App Paths registers executables (ES-DE's winregistrypath rule), under HKCU and HKLM.
constexpr std::string_view kAppPathsKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
constexpr std::string_view kStartDirToken = "%STARTDIR%=";
/// The most of an %INJECT% file that is read. ES-DE reads it until its content passes 4,096 bytes; only line ends could
/// keep a longer prefix within that, and a file whose first MiB is mostly line ends is taken as one ES-DE may inject.
constexpr std::size_t kMaxInjectReadBytes = 1024 * 1024;

std::string inQuotes(std::string_view text) {
    return fmt::format("\"{}\"", text);
}

/// The first `from` in `text` replaced with `to`, as ES-DE's find() and replace() expand %EMUPATH% and %ESPATH% in a
/// core's path (once, where Utils::String::replace would replace them all).
std::string replaceFirst(std::string text, std::string_view from, std::string_view to) {
    if (const std::size_t at = text.find(from); at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

/// The file `path` opens, as the file system names it: 8.3 aliases expanded, symbolic links and junctions followed
/// (GetFinalPathNameByHandleW, without the \\?\ prefix). Nothing when it cannot be opened, such as a program only the
/// probe describes.
std::optional<std::filesystem::path> finalPathOf(const std::filesystem::path& path) {
    const std::wstring target = core::extendedLengthPath(path);
    const HANDLE handle =
        CreateFileW(target.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
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
    CloseHandle(handle);
    if (length == 0 || length >= resolved.size()) {
        return std::nullopt;
    }
    resolved.resize(length);
    if (resolved.starts_with(L"\\\\?\\UNC\\")) {
        resolved = L"\\\\" + resolved.substr(8);
    } else if (resolved.starts_with(L"\\\\?\\")) {
        resolved.erase(0, 4);
    }
    return std::filesystem::path(std::move(resolved));
}

/// How to name `program` when it is refused, by the name found or by the file it opens: an 8.3 alias ("POWERS~1.EXE") or a
/// link named like an emulator would hide a shell's name. Nothing when it is not.
std::optional<std::string> refusedProgram(const std::filesystem::path& program) {
    const auto opened = finalPathOf(program);
    if (!launcher::isRefusedProgram(program) && !(opened && launcher::isRefusedProgram(*opened))) {
        return std::nullopt;
    }
    const std::string name = core::pathToUtf8Lossy(program.filename());
    const std::string openedName = opened ? core::pathToUtf8Lossy(opened->filename()) : name;
    return openedName == name ? name : fmt::format("{} (it opens {})", name, openedName);
}

/// Whether `program` is Windows' command shell, cmd.exe, by the name found or the file it opens: the program of ES-DE's
/// "open the game's file" command (I11 step 13).
bool isCommandShell(const std::filesystem::path& program) {
    const auto shell = [](const std::filesystem::path& path) {
        return core::equalsIgnoreCaseAscii(core::pathToUtf8Lossy(path.filename()), "cmd.exe");
    };
    if (shell(program)) {
        return true;
    }
    const auto opened = finalPathOf(program);
    return opened && shell(*opened);
}

/// At most `maxBytes` from the start of `file`, opened as core::readFileShared() opens it (reading only, sharing
/// everything), and whether more follows. Errors as readFileShared()'s, without OutOfRange: NotFound, Unavailable,
/// InvalidArgument (a folder or a device), Conflict, IoError.
struct FilePrefix {
    std::string bytes;
    bool truncated = false;
};
Expected<FilePrefix> readPrefixShared(const std::filesystem::path& file, std::size_t maxBytes) {
    const std::string context = core::pathToUtf8Lossy(file);
    const std::wstring target = core::extendedLengthPath(file);
    const HANDLE handle =
        CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED) {
            // CreateFileW refuses to open a folder this way with "access denied".
            const DWORD attributes = GetFileAttributesW(target.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                return makeError(ErrorCode::InvalidArgument, "not a regular file (a folder)", context);
            }
        }
        return makeError(core::fileErrorCode(error, file), core::systemErrorMessage(error), context);
    }
    FilePrefix prefix;
    Status status;
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        status = makeError(ErrorCode::InvalidArgument, "not a regular file (a device)", context);
    } else {
        // One byte more than wanted tells whether the file goes on.
        prefix.bytes.resize(maxBytes + 1);
        std::size_t done = 0;
        while (done < prefix.bytes.size()) {
            DWORD read = 0;
            const auto wanted = static_cast<DWORD>(std::min<std::size_t>(prefix.bytes.size() - done, 64 * 1024));
            if (!ReadFile(handle, prefix.bytes.data() + done, wanted, &read, nullptr)) {
                const DWORD error = GetLastError();
                status = makeError(core::fileErrorCode(error, file), core::systemErrorMessage(error), context);
                break;
            }
            if (read == 0) {
                break; // the end of the file
            }
            done += read;
        }
        prefix.bytes.resize(done);
    }
    CloseHandle(handle);
    if (!status) {
        return propagate(status.error());
    }
    if (prefix.bytes.size() > maxBytes) {
        prefix.bytes.resize(maxBytes);
        prefix.truncated = true;
    }
    return prefix;
}

/// The largest Internet Shortcut read (I11 step 13).
constexpr std::size_t kMaxInternetShortcutBytes = 64 * 1024;

/// The URL of an Internet Shortcut (a .url file: the URL= line of its [InternetShortcut] section, both compared
/// ignoring ASCII case, the first one found), its text UTF-8 (a byte order mark skipped), UTF-16 with its byte order
/// mark, or else the system's code page.
Expected<std::string> internetShortcutUrl(const std::filesystem::path& file) {
    auto prefix = readPrefixShared(file, kMaxInternetShortcutBytes);
    if (!prefix) {
        return propagate(prefix.error());
    }
    if (prefix->truncated) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("larger than {} KiB: no Internet Shortcut", kMaxInternetShortcutBytes / 1024));
    }
    std::string text = std::move(prefix->bytes);
    if (text.starts_with("\xFF\xFE")) {
        std::wstring wide((text.size() - 2) / 2, L'\0');
        std::memcpy(wide.data(), text.data() + 2, wide.size() * sizeof(wchar_t));
        auto converted = core::wideToUtf8(wide);
        if (!converted) {
            return makeError(ErrorCode::ValidationFailed, "not valid UTF-16 text");
        }
        text = std::move(*converted);
    } else if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    } else if (!core::isValidUtf8(text)) {
        const int length = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
        if (length <= 0 || MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                                               length) != length) {
            return makeError(ErrorCode::ValidationFailed, "not text");
        }
        text = core::wideToUtf8Lossy(wide);
    }
    bool inSection = false;
    for (std::string_view line : core::split(text, '\n')) {
        line = core::trim(line);
        if (line.starts_with('[')) {
            inSection = core::equalsIgnoreCaseAscii(line, "[InternetShortcut]");
            continue;
        }
        const std::size_t equals = line.find('=');
        if (inSection && equals != std::string_view::npos &&
            core::equalsIgnoreCaseAscii(core::trim(line.substr(0, equals)), "URL")) {
            return std::string(core::trim(line.substr(equals + 1)));
        }
    }
    return makeError(ErrorCode::ValidationFailed, "no URL= line in an [InternetShortcut] section");
}

/// A shortcut's argument string split as CommandLineToArgvW splits the arguments after a program.
std::vector<std::string> splitArguments(std::string_view arguments) {
    if (core::trim(arguments).empty()) {
        return {};
    }
    std::vector<std::string> elements = splitCommandLine("x " + std::string(arguments));
    elements.erase(elements.begin()); // the program's place
    return elements;
}

/// ES-DE's preferred form of a path's text (Utils::FileSystem::getPreferredPath on Windows): '\' between the elements.
std::string preferred(std::string text) {
    std::replace(text.begin(), text.end(), '/', '\\');
    return text;
}

/// ES-DE's generic form of a path's text (Utils::FileSystem::getGenericPath): no \\?\ prefix, '/' between the
/// elements, repeated separators collapsed, no trailing one; a network path keeps its leading pair, as
/// esdeGenericText() keeps it.
std::string genericText(std::string_view text) {
    std::string slashed(text);
    if (slashed.starts_with("\\\\?\\")) {
        slashed.erase(0, 4);
    }
    std::replace(slashed.begin(), slashed.end(), '\\', '/');
    const std::size_t keep = slashed.starts_with("//") ? 2 : 0;
    std::string generic = slashed.substr(0, keep);
    for (std::size_t at = keep; at < slashed.size(); ++at) {
        if (slashed[at] == '/' && !generic.empty() && generic.back() == '/') {
            continue;
        }
        generic += slashed[at];
    }
    while (generic.size() > std::max<std::size_t>(keep, 1) && generic.back() == '/') {
        generic.pop_back();
    }
    return generic;
}

/// A folder's text as ES-DE puts it into a command (getParent and the like, then '\'): "E:\Programmi\Retroarch".
std::string folderText(const std::filesystem::path& folder) {
    return preferred(esdeGenericText(folder));
}

/// ES-DE's home folder as '~' expands to it (Utils::FileSystem::getHomePath: the generic form).
std::string homeText(const TranslationContext& context) {
    return esdeGenericText(context.profile.esdeHome.parent_path());
}

/// The find rules of the emulator `name`, or null when es_find_rules.xml defines none, or none of the four types ES-DE
/// searches for a program on Windows (its NO_RULES: "missing emulator find rules configuration").
const FindRuleSet* emulatorRules(const TranslationContext& context, std::string_view name) {
    const FindRuleSet* rules = context.findRules != nullptr ? context.findRules->findEmulator(name) : nullptr;
    if (rules == nullptr) {
        return nullptr;
    }
    for (const FindRuleType type : {FindRuleType::WinRegistryPath, FindRuleType::WinRegistryValue,
                                    FindRuleType::SystemPath, FindRuleType::StaticPath}) {
        if (!rules->entries(type).empty()) {
            return rules;
        }
    }
    return nullptr;
}

/// The find rules of the core `name`, or null when es_find_rules.xml defines no corepath entry for it (ES-DE's
/// "missing core configuration").
const FindRuleSet* coreRules(const TranslationContext& context, std::string_view name) {
    const FindRuleSet* rules = context.findRules != nullptr ? context.findRules->findCore(name) : nullptr;
    return rules != nullptr && !rules->entries(FindRuleType::CorePath).empty() ? rules : nullptr;
}

/// Evaluates one emulator's find rules in ES-DE's order (FileData::findEmulator), on the probe.
class RuleSearch {
public:
    RuleSearch(const TranslationContext& context, FindResult& result) : m_context(context), m_result(result) {}

    /// Each returns true when the search ends there: found, or undecided.
    bool appPath(const std::string& entry) {
        const std::string key = std::string(kAppPathsKey) + entry;
        const auto root = registryRoot(key);
        if (!root) {
            attempt(FindRuleType::WinRegistryPath, entry, "not registered in App Paths");
            return false;
        }
        const auto value = probe().registryString(*root, key, "");
        if (!value) {
            attempt(FindRuleType::WinRegistryPath, entry,
                    fmt::format("its App Paths key in {} holds no path", core::toString(*root)));
            return false;
        }
        return candidate(FindRuleType::WinRegistryPath, entry, *value, std::string(core::toString(*root)));
    }

    bool registryValue(const std::string& entry) {
        // "<key>\<value>|<suffix>": ES-DE takes the key and the value name as a path's parent and file name.
        const std::size_t pipe = entry.find('|');
        const std::string generic = genericText(entry.substr(0, pipe));
        const std::string suffix = pipe == std::string::npos ? std::string() : entry.substr(pipe + 1);
        const std::size_t slash = generic.rfind('/');
        const std::string key = preferred(slash == std::string::npos ? generic : generic.substr(0, slash));
        const std::string valueName = slash == std::string::npos ? generic : generic.substr(slash + 1);
        const auto root = registryRoot(key);
        if (!root) {
            attempt(FindRuleType::WinRegistryValue, entry, fmt::format("no registry key {}", key));
            return false;
        }
        const auto value = probe().registryString(*root, key, valueName);
        if (!value) {
            attempt(FindRuleType::WinRegistryValue, entry,
                    fmt::format("{}\\{} has no string value {}", core::toString(*root), key, inQuotes(valueName)));
            return false;
        }
        // The suffix is appended verbatim: "InstallLocation|mGBA.exe" relies on the value's trailing backslash.
        return candidate(FindRuleType::WinRegistryValue, entry, *value + suffix, std::string(core::toString(*root)));
    }

    bool systemPath(const std::string& entry) {
        // ES-DE's SearchPathW looks in its own folder first, then the system folders, its current folder and PATH.
        // Retroverse searches the install folder, System32, the Windows folder and PATH's absolute entries: never its
        // own folder or the current one, which may lie in a ROM tree holding emulators named like the entries.
        if (!m_context.profile.installFolder) {
            undecided(fmt::format("systempath {}: ES-DE searches its install folder first, which is not known (give "
                                  "it to library create --install)",
                                  inQuotes(entry)));
            return true;
        }
        const auto name = core::tryPathFromUtf8(entry);
        if (!name || entry.empty()) {
            attempt(FindRuleType::SystemPath, entry, "not a file name");
            return false;
        }
        if (!name->is_absolute() && (name->has_root_name() || name->has_root_directory())) {
            // "D:emu.exe" or "\emu.exe": relative to a drive's current folder or to the current drive's root, which
            // Windows would take from Retroverse's own state.
            attempt(FindRuleType::SystemPath, entry, "not a file name (a path relative to a drive's current folder)");
            return false;
        }
        std::vector<std::filesystem::path> folders{*m_context.profile.installFolder, probe().system32Folder(),
                                                   probe().windowsFolder()};
        const std::vector<std::filesystem::path> pathEntries = probe().pathEntries();
        folders.insert(folders.end(), pathEntries.begin(), pathEntries.end());
        // SearchPathW's default extension: added when the name has none.
        const bool hasExtension = name->filename().native().find(L'.') != std::wstring::npos;
        for (const std::filesystem::path& folder : folders) {
            std::filesystem::path file = name->is_absolute() ? *name : folder / *name;
            if (!hasExtension) {
                file += L".exe";
            }
            file = file.lexically_normal();
            if (!file.is_absolute()) {
                continue; // a relative folder: never resolved against the current one
            }
            if (probe().fileExists(file)) {
                m_result.found = FoundLocation{std::move(file), std::string(toString(FindRuleType::SystemPath)), entry,
                                               core::pathToUtf8Lossy(folder)};
                return true;
            }
            if (name->is_absolute()) {
                break;
            }
        }
        attempt(FindRuleType::SystemPath, entry,
                fmt::format("not in the ES-DE install folder, System32, the Windows folder or the {} folder(s) of PATH",
                            pathEntries.size()));
        return false;
    }

    bool staticPath(const std::string& entry) {
        const std::size_t pipe = entry.find('|');
        std::string text = esdeReplace(entry.substr(0, pipe), "~", homeText(m_context));
        if (text.find("%ESPATH%") != std::string::npos) {
            if (!m_context.profile.installFolder) {
                undecided(fmt::format("staticpath {} uses %ESPATH%, ES-DE's install folder, which is not known (give "
                                      "it to library create --install)",
                                      inQuotes(entry)));
                return true;
            }
            text = esdeReplace(std::move(text), "%ESPATH%", esdeGenericText(*m_context.profile.installFolder));
        }
        if (text.find("%ROMPATH%") != std::string::npos) {
            if (!m_context.romRoot.path) {
                undecided(fmt::format("staticpath {} uses %ROMPATH%, the ROM root, which is not resolved",
                                      inQuotes(entry)));
                return true;
            }
            text = esdeReplace(std::move(text), "%ROMPATH%", m_context.romRoot.esdeText);
        }
        text = preferred(std::move(text));
        std::optional<std::filesystem::path> file;
        if (text.find('*') != std::string::npos) {
            // A wildcard in the file name only: ES-DE lists the folder and matches the names with a regular expression.
            const std::size_t separator = text.rfind('\\');
            const std::string folder = separator == std::string::npos ? std::string() : text.substr(0, separator);
            const std::string pattern = text.substr(separator == std::string::npos ? 0 : separator + 1);
            const auto folderPath = folder.find('*') == std::string::npos ? folderFromEsdeText(folder) : std::nullopt;
            if (!folderPath) {
                attempt(FindRuleType::StaticPath, entry,
                        fmt::format("{}: a wildcard is matched in the last element of an absolute path only", text));
                return false;
            }
            file = firstMatch(*folderPath, pattern);
            if (!file) {
                attempt(FindRuleType::StaticPath, entry,
                        fmt::format("no file of {} matches {}", core::pathToUtf8Lossy(*folderPath), inQuotes(pattern)));
                return false;
            }
        } else {
            file = folderFromEsdeText(text);
            if (!file) {
                attempt(FindRuleType::StaticPath, entry, fmt::format("{} is not an absolute path", inQuotes(text)));
                return false;
            }
        }
        if (!probe().fileExists(*file)) {
            attempt(FindRuleType::StaticPath, entry, fmt::format("{} does not exist", core::pathToUtf8Lossy(*file)));
            return false;
        }
        if (pipe != std::string::npos) {
            // ES-DE replaces the program with the text after the '|' ("Replacing emulator in staticpath rule").
            undecided(fmt::format("staticpath {}: {} exists, and ES-DE then runs {} instead, which Retroverse does "
                                  "not run",
                                  inQuotes(entry), core::pathToUtf8Lossy(*file), inQuotes(entry.substr(pipe + 1))));
            return true;
        }
        m_result.found = FoundLocation{std::move(*file), std::string(toString(FindRuleType::StaticPath)), entry, {}};
        return true;
    }

private:
    [[nodiscard]] const core::ISystemProbe& probe() const { return *m_context.probe; }

    /// HKCU when the key exists there, else HKLM when it does (ES-DE opens HKLM only when HKCU's key cannot be).
    [[nodiscard]] std::optional<core::RegistryRoot> registryRoot(const std::string& key) const {
        if (probe().registryKeyExists(core::RegistryRoot::CurrentUser, key)) {
            return core::RegistryRoot::CurrentUser;
        }
        if (probe().registryKeyExists(core::RegistryRoot::LocalMachine, key)) {
            return core::RegistryRoot::LocalMachine;
        }
        return std::nullopt;
    }

    /// The registry's text of a file: found when it names an existing file (quotes are not removed: ES-DE keeps them,
    /// and a quoted value names no file for it either).
    bool candidate(FindRuleType type, const std::string& entry, const std::string& text, std::string detail) {
        auto file = folderFromEsdeText(text);
        if (!file) {
            attempt(type, entry, fmt::format("{} is not an absolute path", inQuotes(text)));
            return false;
        }
        if (!probe().fileExists(*file)) {
            attempt(type, entry, fmt::format("{} does not exist", core::pathToUtf8Lossy(*file)));
            return false;
        }
        m_result.found = FoundLocation{std::move(*file), std::string(toString(type)), entry, std::move(detail)};
        return true;
    }

    /// The first file of `folder`, in ordinal order, whose name matches `pattern` as ES-DE matches it: a regular
    /// expression where '*' is ".*", '(', ')', '[' and ']' are escaped and the rest ('.' among it) keeps its meaning,
    /// matched case-sensitively against the whole name.
    [[nodiscard]] std::optional<std::filesystem::path> firstMatch(const std::filesystem::path& folder,
                                                                  std::string_view pattern) const {
        std::string expression;
        for (const char c : pattern) {
            if (c == '*') {
                expression += ".*";
            } else {
                if (c == '(' || c == ')' || c == '[' || c == ']') {
                    expression += '\\';
                }
                expression += c;
            }
        }
        try {
            const std::regex regex(expression);
            for (const std::filesystem::path& file : probe().filesMatching(folder, "*")) {
                const auto name = core::tryPathToUtf8(file.filename());
                if (name && std::regex_match(*name, regex)) {
                    return file;
                }
            }
        } catch (const std::regex_error&) {
            // An expression ES-DE's std::regex refuses as well: nothing matches.
        }
        return std::nullopt;
    }

    void attempt(FindRuleType type, std::string_view entry, std::string_view why) {
        m_result.attempts.push_back(fmt::format("{} {}: {}", toString(type), inQuotes(entry), why));
    }

    void undecided(std::string why) { m_result.undecided = std::move(why); }

    const TranslationContext& m_context;
    FindResult& m_result;
};

/// What an %INJECT% file holds as ES-DE reads it.
enum class InjectContent : std::uint8_t {
    None,       ///< no such file (or not a regular file): nothing injected
    Empty,      ///< nothing once its line ends are removed: nothing injected
    TooLong,    ///< more than kMaxInjectBytes: ES-DE warns and injects nothing
    Injected,   ///< ES-DE injects it
    Unreadable, ///< it cannot be read: what ES-DE does is not known
};

/// Reads an %INJECT% file as ES-DE does: line by line in text mode (so up to a Ctrl+Z), each line's '\r' removed, the
/// lines joined without a separator, until the content passes kMaxInjectBytes. Only a prefix is read: a large file
/// is decided by its first kMaxInjectReadBytes (too long as soon as its content passes the limit).
InjectContent injectContent(const std::filesystem::path& file) {
    auto prefix = readPrefixShared(file, kMaxInjectReadBytes);
    if (!prefix) {
        switch (prefix.error().code) {
        case ErrorCode::NotFound:
        case ErrorCode::InvalidArgument: // a folder or a device
            return InjectContent::None;
        default:
            return InjectContent::Unreadable;
        }
    }
    std::size_t bytes = 0;
    for (const char c : prefix->bytes) {
        if (c == '\x1A') {
            return bytes == 0 ? InjectContent::Empty : InjectContent::Injected;
        }
        if (c != '\r' && c != '\n' && ++bytes > kMaxInjectBytes) {
            return InjectContent::TooLong;
        }
    }
    if (prefix->truncated) {
        return InjectContent::Injected; // line ends mostly, and more to come: what ES-DE does is not decided yet
    }
    return bytes == 0 ? InjectContent::Empty : InjectContent::Injected;
}

/// The values of the tokens that depend on the game, as ES-DE's launchGame() computes them from the game's path.
struct GameValues {
    std::string rom;      ///< %ROM%: the path, '\' between the elements (a folder game's inner file)
    std::string romRaw;   ///< %ROMRAW% and %INJECT%=%ROM%: the game's path (a folder game's folder)
    std::string baseName; ///< %BASENAME%
    std::string fileName; ///< %FILENAME%
    std::string gameDir;  ///< %GAMEDIR%, %GAMEDIRRAW%: the folder holding %ROM%
    std::string gameDirGeneric;
    std::string title; ///< for the request's label
};

/// ES-DE's text of the ROM folder of `system` (SystemData::loadConfig: %ROMPATH% replaced, then '~', '\' turned into
/// '/' and repeated separators collapsed); an override root comes in after the path's '~' is replaced, as
/// systemRomFolder() puts it. Nothing when it gives no absolute folder.
std::optional<std::string> romFolderText(const TranslationContext& context, const SystemDefinition& system) {
    const ResolvedRoot& root = context.romRoot;
    if (system.path.find("%ROMPATH%") != std::string::npos && !root.path) {
        return std::nullopt;
    }
    const std::string home = homeText(context);
    const std::string text =
        root.source == RootSource::Override
            ? esdeReplace(esdeReplace(system.path, "~", home), "%ROMPATH%", root.esdeText)
            : esdeReplace(esdeReplace(system.path, "%ROMPATH%", root.esdeText), "~", home);
    if (!folderFromEsdeText(text)) {
        return std::nullopt;
    }
    return genericText(text);
}

/// The values for `game` in ES-DE's text of its ROM folder. `throughInnerFile`: the game is a folder holding a file of
/// its own name, which ES-DE launches instead (launchGame's "directories interpreted as files" shortcut).
GameValues gameValuesOf(const std::string& romFolder, const catalog::LaunchGame& game, bool throughInnerFile) {
    GameValues values;
    std::string relative = game.relativePath;
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const std::string generic = genericText(romFolder + "/" + relative);
    values.rom = preferred(generic);
    values.romRaw = values.rom;
    const std::size_t slash = generic.rfind('/');
    const std::string name = slash == std::string::npos ? generic : generic.substr(slash + 1);
    const std::string folder = slash == std::string::npos ? std::string() : generic.substr(0, slash);
    values.gameDirGeneric = folder;
    values.gameDir = preferred(folder);
    // Utils::FileSystem::getStem: the name up to its last '.', a folder's whole name; getExtension: from the last
    // '.', "." when there is none. ES-DE's %FILENAME% is the two joined, so a name without a dot gains one.
    const std::size_t dot = name.find_last_of('.');
    values.baseName = name == "." || game.directory || dot == std::string::npos ? name : name.substr(0, dot);
    const std::string extension = name == "." ? std::string(".") : dot == std::string::npos ? std::string(".")
                                                                                              : name.substr(dot);
    if (throughInnerFile) {
        // romPath becomes the inner file, so its parent (%GAMEDIR%, a relative %INJECT% file's folder) is the game's
        // folder itself; baseName is cut at its first '.'; romRaw stays the folder.
        values.rom = preferred(generic + "/" + name);
        values.gameDirGeneric = generic;
        values.gameDir = preferred(generic);
        values.baseName = name.substr(0, name.find('.'));
    }
    values.fileName = values.baseName + extension;
    values.title = game.title.empty() ? name : game.title;
    return values;
}

/// Whether the folder `folder` (a \\?\ path) lists a regular file, or a symbolic link, named exactly `name` (compared
/// case-sensitively, as ES-DE compares getFileName()s): ES-DE then launches the folder game through it.
bool holdsFileNamed(const std::filesystem::path& folder, const std::filesystem::path& name) {
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        const std::filesystem::directory_entry& entry = *entries;
        if (entry.path().filename().native() != name.native()) {
            continue;
        }
        std::error_code status;
        return entry.is_symlink(status) || entry.is_regular_file(status);
    }
    return false;
}

/// One command's translation (translateCommand()).
class CommandTranslation {
public:
    CommandTranslation(const TranslationContext& context, const SystemDefinition& system,
                       const SystemCommand& command, const catalog::LaunchGame* game)
        : m_context(context), m_system(system), m_command(command), m_game(game) {}

    LaunchTranslation run() {
        if (m_command.placeholder) {
            block(LaunchReadiness::RequiresConfiguration, "command-placeholder",
                  fmt::format("the command {} is a placeholder: ES-DE has no emulator for {} yet",
                              inQuotes(m_command.text), m_system.name.str()));
            return std::move(m_translation);
        }
        const CommandTemplate parsed = parseCommandTemplate(m_command.text);
        m_opensFile = opensGameFile(parsed);
        if (m_game != nullptr) {
            prepareGame();
        }
        for (std::size_t i = 0; i < parsed.programIndex; ++i) {
            leadingElement(parsed.elements[i]);
        }
        if (parsed.programIndex >= parsed.elements.size()) {
            block(LaunchReadiness::RequiresConfiguration, "program-missing",
                  fmt::format("the command {} names no program", inQuotes(m_command.text)));
            return std::move(m_translation);
        }
        const CommandElement& program = parsed.elements[parsed.programIndex];
        locateProgram(program);
        if (m_opensFile && m_translation.program) {
            openGameFile(m_startDir ? startFolder(*m_startDir) : std::filesystem::path());
            return std::move(m_translation);
        }
        m_opensFile = false; // the shell not found: what ES-DE would refuse, reported
        if (m_escapeSpecials) {
            unsupported(*m_escapeSpecials, " (it escapes what cmd.exe reads in the values)");
        }

        std::vector<std::string> arguments;
        for (std::size_t i = parsed.programIndex + 1; i < parsed.elements.size(); ++i) {
            if (auto argument = expandElement(parsed.elements[i])) {
                arguments.push_back(std::move(*argument));
            }
        }
        std::filesystem::path workingDirectory;
        if (m_startDir) {
            workingDirectory = startFolder(*m_startDir);
        }
        if (workingDirectory.empty() && m_translation.program) {
            workingDirectory = m_translation.program->path.parent_path();
        }
        if (m_translation.program) {
            finish(program, std::move(arguments), std::move(workingDirectory));
        }
        return std::move(m_translation);
    }

private:
    void block(LaunchReadiness readiness, std::string code, std::string message) {
        m_translation.blockers.push_back(ReadinessBlocker{readiness, std::move(code), std::move(message)});
    }

    /// A token Retroverse does not run: requires launcher configuration, named once.
    void unsupported(const CommandToken& token, std::string_view what) {
        if (!m_reported.insert(token.text).second) {
            return;
        }
        block(LaunchReadiness::RequiresConfiguration, "token-unsupported",
              fmt::format("the command uses {}{}, which Retroverse does not run: choose another command or an "
                          "emulator profile for {}",
                          token.text, what, m_system.name.str()));
    }

    void prepareGame() {
        const auto folder = romFolderText(m_context, m_system);
        if (!folder) {
            block(LaunchReadiness::SourceUnavailable, "rom-folder-unresolved",
                  fmt::format("the ROM folder of {} ({}) gives no absolute folder", m_system.name.str(),
                              inQuotes(m_system.path)));
            return;
        }
        bool throughInnerFile = false;
        if (m_game->directory) {
            // ES-DE lists a folder game and launches the file inside that has exactly the folder's name.
            if (const auto romFolder = folderFromEsdeText(*folder)) {
                if (const auto gameFolder = core::extendedLengthPathBelow(*romFolder, m_game->relativePath)) {
                    throughInnerFile = holdsFileNamed(*gameFolder, gameFolder->filename());
                }
            }
        }
        m_values = gameValuesOf(*folder, *m_game, throughInnerFile);
        if (throughInnerFile) {
            m_translation.notes.push_back(fmt::format("the folder game holds a file of its own name, which ES-DE "
                                                      "launches: %ROM% is {}, %ROMRAW% the folder",
                                                      m_values->rom));
        }
    }

    /// What leads the command and ES-DE removes before it launches it: flags and the start folder.
    void leadingElement(const CommandElement& element) {
        if (element.text == "%HIDEWINDOW%") {
            m_hideWindow = true;
        } else if (element.text.starts_with(kStartDirToken)) {
            startDirElement(element);
        } else if (!element.tokens.empty()) {
            flag(element.tokens.front());
        }
    }

    void startDirElement(const CommandElement& element) {
        if (m_startDir) {
            // ES-DE takes the first %STARTDIR% out of the command and leaves a second one in it as text.
            if (m_reported.insert("%STARTDIR%=repeated").second) {
                block(LaunchReadiness::RequiresConfiguration, "start-folder-repeated",
                      "the command names %STARTDIR% twice: ES-DE uses the first and passes the second on as text");
            }
            return;
        }
        m_startDir = element;
    }

    void flag(const CommandToken& token) {
        if (token.name == "HIDEWINDOW") {
            m_hideWindow = true;
        } else if (token.name == "RUNINBACKGROUND") {
            // ES-DE keeps running beside the program: Retroverse follows the game's window (I11 step 13).
            m_runInBackground = true;
        } else if (token.name == "ESCAPESPECIALS") {
            // Meaningless when Retroverse opens the game's file itself; reported for any other command (once
            // locateProgram() has told, for a command shaped like that one).
            if (!m_opensFile) {
                unsupported(token, " (it escapes what cmd.exe reads in the values)");
            } else if (!m_escapeSpecials) {
                m_escapeSpecials = token;
            }
        }
    }

    /// ES-DE's "open the game's file" (I11 step 13, ADR-0020 §3): the program one %EMULATOR_X% element, then exactly
    /// "/C" (any case) and "%ROM%", flags aside. Whether X finds cmd.exe is locateProgram()'s to tell.
    static bool opensGameFile(const CommandTemplate& parsed) {
        if (parsed.programIndex >= parsed.elements.size()) {
            return false;
        }
        const CommandElement& program = parsed.elements[parsed.programIndex];
        const bool emulator = program.tokens.size() == 1 && program.tokens.front().kind == CommandTokenKind::Emulator &&
                              program.tokens.front().offset == 0 &&
                              program.tokens.front().text.size() == program.text.size();
        if (!emulator) {
            return false;
        }
        std::vector<std::string_view> rest;
        for (std::size_t i = parsed.programIndex + 1; i < parsed.elements.size(); ++i) {
            const CommandElement& element = parsed.elements[i];
            const bool flagOnly = element.tokens.size() == 1 && element.tokens.front().kind == CommandTokenKind::Flag &&
                                  element.tokens.front().text.size() == element.text.size();
            if (!flagOnly) {
                rest.push_back(element.text);
            }
        }
        return rest.size() == 2 && core::equalsIgnoreCaseAscii(rest[0], "/C") && rest[1] == "%ROM%";
    }

    /// The request "open the game's file" makes: Retroverse opens the file itself, by its extension, and cmd.exe never
    /// runs. A shortcut (.lnk, read through the probe, never resolved) starts its program with its arguments in its
    /// working folder; a program (.exe, .com) starts as itself; an Internet Shortcut (.url) activates its URL when its
    /// scheme is one of emulators.json's protocols. Anything else requires launcher configuration: a batch file, a
    /// document, a disc image. `startFolder` is %STARTDIR%'s (empty: none).
    void openGameFile(const std::filesystem::path& startFolder) {
        m_translation.program.reset(); // the shell is never started
        if (m_game == nullptr) {
            m_translation.notes.push_back(
                "Retroverse opens each game's file itself, never through cmd.exe: a shortcut (.lnk) to a program, a "
                "program (.exe), an Internet Shortcut (.url) of a protocol emulators.json lists");
            return;
        }
        if (!m_values) {
            return; // the ROM folder is not known: reported
        }
        const std::string& shown = m_values->rom;
        const auto file = folderFromEsdeText(shown);
        if (!file) {
            block(LaunchReadiness::RequiresConfiguration, "file-not-opened",
                  fmt::format("the game's path {} is not absolute", inQuotes(shown)));
            return;
        }
        if (m_game->directory && m_values->rom == m_values->romRaw) {
            block(LaunchReadiness::RequiresConfiguration, "file-not-opened",
                  fmt::format("{} is a folder, which Retroverse does not open: choose another command for {}", shown,
                              m_system.name.str()));
            return;
        }
        const std::string extension = core::toLowerAscii(core::pathToUtf8Lossy(file->extension()));
        launcher::LaunchRequest request;
        if (extension == ".lnk") {
            auto shortcut = m_context.probe->readShortcut(*file);
            if (!shortcut) {
                block(LaunchReadiness::RequiresConfiguration, "shortcut-unreadable",
                      fmt::format("the shortcut {} cannot be read: {}", shown, shortcut.error().message));
                return;
            }
            const std::filesystem::path& target = shortcut->target;
            const std::string targetShown = core::pathToUtf8Lossy(target);
            m_translation.notes.push_back(fmt::format("the shortcut {} opens {}", shown, targetShown));
            const std::string targetExtension = core::toLowerAscii(core::pathToUtf8Lossy(target.extension()));
            if (targetExtension == ".bat" || targetExtension == ".cmd") {
                block(LaunchReadiness::RequiresConfiguration, "batch-file",
                      fmt::format("the shortcut {} opens the batch file {}, which runs through cmd.exe: Retroverse "
                                  "never starts it",
                                  shown, targetShown));
                return;
            }
            if (targetExtension != ".exe" && targetExtension != ".com") {
                block(LaunchReadiness::RequiresConfiguration, "shortcut-target",
                      fmt::format("the shortcut {} opens {}, which is no program: Retroverse opens shortcuts to "
                                  "programs only",
                                  shown, targetShown));
                return;
            }
            if (!m_context.probe->fileExists(target)) {
                block(LaunchReadiness::EmulatorNotFound, "emulator-not-found",
                      fmt::format("the program the shortcut {} opens, {}, does not exist", shown, targetShown));
                return;
            }
            if (const auto refused = refusedProgram(target)) {
                block(LaunchReadiness::RequiresConfiguration, "program-refused",
                      fmt::format("the shortcut {} opens {}, a command interpreter or a launcher, which Retroverse "
                                  "never starts",
                                  shown, *refused));
                return;
            }
            if (shortcut->runAsAdministrator) {
                m_translation.notes.push_back("the shortcut asks for administrator rights, which Retroverse does not "
                                              "request: the program starts without them");
            }
            request.executable = target;
            request.arguments = splitArguments(shortcut->arguments);
            if (!shortcut->workingDirectory.empty() && shortcut->workingDirectory.is_absolute()) {
                request.workingDirectory = shortcut->workingDirectory;
            } else {
                request.workingDirectory = !startFolder.empty() ? startFolder : target.parent_path();
            }
            m_translation.program = FoundLocation{target, "shortcut", shown, {}};
        } else if (extension == ".exe" || extension == ".com") {
            if (const auto refused = refusedProgram(*file)) {
                block(LaunchReadiness::RequiresConfiguration, "program-refused",
                      fmt::format("{} is a command interpreter or a launcher, which Retroverse never starts", *refused));
                return;
            }
            request.executable = *file;
            request.workingDirectory = !startFolder.empty() ? startFolder : file->parent_path();
            // A console program runs in cmd.exe's console, which %HIDEWINDOW% hides.
            request.hideConsoleWindow = m_hideWindow;
            m_translation.program = FoundLocation{*file, "game-file", shown, {}};
        } else if (extension == ".url") {
            auto url = internetShortcutUrl(*file);
            if (!url) {
                block(LaunchReadiness::RequiresConfiguration, "shortcut-unreadable",
                      fmt::format("the Internet Shortcut {} cannot be read: {}", shown, url.error().message));
                return;
            }
            const std::optional<std::string> scheme = launcher::activationScheme(*url);
            const launcher::ProtocolRule* rule = nullptr;
            if (scheme && m_context.protocols != nullptr) {
                const auto found = std::find_if(m_context.protocols->begin(), m_context.protocols->end(),
                                                [&](const launcher::ProtocolRule& entry) {
                                                    return core::equalsIgnoreCaseAscii(entry.scheme, *scheme);
                                                });
                rule = found == m_context.protocols->end() ? nullptr : &*found;
            }
            if (rule == nullptr) {
                block(LaunchReadiness::RequiresConfiguration, "protocol-not-listed",
                      fmt::format("the Internet Shortcut {} opens {}, a scheme emulators.json's \"protocols\" do "
                                  "not list: Retroverse never activates it",
                                  shown, scheme ? inQuotes(*scheme + ":") : std::string("an address without one")));
                return;
            }
            request.activationUrl = std::move(*url);
            request.waitStrategy = launcher::WaitStrategy::FollowWindow;
            request.launcherProcesses = rule->launcherProcesses;
            m_translation.program = FoundLocation{*file, "internet-shortcut", shown, {}};
        } else if (extension == ".bat" || extension == ".cmd") {
            block(LaunchReadiness::RequiresConfiguration, "batch-file",
                  fmt::format("{} is a batch file, which runs through cmd.exe: Retroverse never starts it", shown));
            return;
        } else {
            block(LaunchReadiness::RequiresConfiguration, "file-not-opened",
                  fmt::format("Retroverse opens a game's file itself only when it is a shortcut (.lnk), a program "
                              "(.exe) or an Internet Shortcut (.url), and {} is none: choose another command for {}",
                              shown, m_system.name.str()));
            return;
        }
        if (m_runInBackground) {
            request.waitStrategy = launcher::WaitStrategy::FollowWindow;
        }
        request.label = fmt::format("{} with {}", m_values->title,
                                    m_command.label.empty() ? std::string("its own program") : m_command.label);
        if (const Status valid = launcher::validateLaunchRequest(request); !valid) {
            block(LaunchReadiness::RequiresConfiguration, "invalid-request", valid.error().toString());
        }
        m_translation.request = std::move(request);
    }

    /// The program: exactly one %EMULATOR_X% element, or a literal absolute path whose tokens are '~' and %ESPATH%.
    void locateProgram(const CommandElement& element) {
        const auto& tokens = element.tokens;
        const bool emulator = tokens.size() == 1 && tokens.front().kind == CommandTokenKind::Emulator &&
                              tokens.front().offset == 0 && tokens.front().text.size() == element.text.size();
        if (emulator) {
            m_emulatorName = tokens.front().target;
            m_translation.emulator = m_emulatorName;
            FindResult found = findEmulator(m_context, m_emulatorName);
            m_translation.attempts.insert(m_translation.attempts.end(), found.attempts.begin(), found.attempts.end());
            if (found.noRules) {
                block(LaunchReadiness::RequiresConfiguration, "find-rules-missing",
                      fmt::format("no find rule defines the emulator {} (es_find_rules.xml)", m_emulatorName));
            } else if (found.undecided) {
                block(LaunchReadiness::RequiresConfiguration, "find-rule-undecided", *found.undecided);
            } else if (found.found) {
                m_translation.program = std::move(found.found);
            } else {
                block(LaunchReadiness::EmulatorNotFound, "emulator-not-found",
                      fmt::format("the emulator {} is not on this machine (its find rules found nothing)",
                                  m_emulatorName));
            }
        } else {
            const bool literal = std::all_of(tokens.begin(), tokens.end(), [](const CommandToken& token) {
                return token.kind == CommandTokenKind::HomeFolder ||
                       (token.kind == CommandTokenKind::Variable && token.name == "ESPATH");
            });
            if (!literal) {
                block(LaunchReadiness::RequiresConfiguration, "program-not-allowed",
                      fmt::format("the program {} is neither one %EMULATOR_X% element nor an absolute path (its only "
                                  "tokens '~' and %ESPATH%)",
                                  inQuotes(element.text)));
                return;
            }
            std::string text = esdeReplace(element.text, "~", homeText(m_context));
            if (text.find("%ESPATH%") != std::string::npos) {
                if (!m_context.profile.installFolder) {
                    block(LaunchReadiness::RequiresConfiguration, "install-unknown",
                          "the program uses %ESPATH%, ES-DE's install folder, which is not known (give it to library "
                          "create --install)");
                    return;
                }
                text = esdeReplace(std::move(text), "%ESPATH%", folderText(*m_context.profile.installFolder));
            }
            auto path = folderFromEsdeText(text);
            if (!path) {
                block(LaunchReadiness::RequiresConfiguration, "program-not-allowed",
                      fmt::format("the program {} is not an absolute path", inQuotes(element.text)));
                return;
            }
            m_emulatorName = core::pathToUtf8Lossy(path->filename());
            if (!m_context.probe->fileExists(*path)) {
                m_translation.attempts.push_back(
                    fmt::format("the command's program {} does not exist", core::pathToUtf8Lossy(*path)));
                block(LaunchReadiness::EmulatorNotFound, "emulator-not-found",
                      fmt::format("the program the command names, {}, does not exist", core::pathToUtf8Lossy(*path)));
                return;
            }
            m_translation.program = FoundLocation{std::move(*path), "command", element.text, {}};
        }
        if (m_translation.program) {
            m_programFolder = m_translation.program->path.parent_path();
            // "Open the game's file" needs the shell to be cmd.exe, which then never starts (openGameFile()).
            if (m_opensFile && isCommandShell(m_translation.program->path)) {
                return;
            }
            m_opensFile = false;
            // The request keeps the path as found, which Windows opens alike.
            if (const auto refused = refusedProgram(m_translation.program->path)) {
                block(LaunchReadiness::RequiresConfiguration, "program-refused",
                      fmt::format("{} is a command interpreter or a launcher, which Retroverse never starts: choose "
                                  "another command or an emulator profile for {}",
                                  *refused, m_system.name.str()));
            }
        }
    }

    /// The value of a variable token in an argument.
    std::string variable(const CommandToken& token) {
        const std::string& name = token.name;
        if (name == "ROM") {
            return m_values ? m_values->rom : token.text;
        }
        if (name == "ROMRAW") {
            return m_values ? m_values->romRaw : token.text;
        }
        if (name == "BASENAME") {
            return m_values ? m_values->baseName : token.text;
        }
        if (name == "FILENAME") {
            return m_values ? m_values->fileName : token.text;
        }
        if (name == "GAMEDIR" || name == "GAMEDIRRAW") {
            return m_values ? m_values->gameDir : token.text;
        }
        if (name == "ROMPATH") {
            if (!m_context.romRoot.path) {
                if (m_reported.insert(token.text).second) {
                    block(LaunchReadiness::RequiresConfiguration, "rom-root-unknown",
                          "the command uses %ROMPATH%, the ROM root, which is not resolved");
                }
                return token.text;
            }
            return preferred(genericText(m_context.romRoot.esdeText));
        }
        if (name == "EMUDIR") {
            return m_programFolder ? folderText(*m_programFolder) : token.text;
        }
        if (name == "ESPATH") {
            if (!m_context.profile.installFolder) {
                if (m_reported.insert(token.text).second) {
                    block(LaunchReadiness::RequiresConfiguration, "install-unknown",
                          "the command uses %ESPATH%, ES-DE's install folder, which is not known (give it to library "
                          "create --install)");
                }
                return token.text;
            }
            return folderText(*m_context.profile.installFolder);
        }
        if (name == "EMUPATH") {
            unsupported(token, " (ES-DE replaces it in core paths only)");
        } else {
            unsupported(token, " (ES-DE replaces it in %STARTDIR% only)");
        }
        return token.text;
    }

    /// An argument: its tokens replaced; nothing when it is dropped (flags alone, the start folder, an %INJECT%
    /// element with nothing before it).
    std::optional<std::string> expandElement(const CommandElement& element) {
        std::string out;
        std::size_t at = 0;
        bool kept = false; // something of the element stays: text or a value
        for (const CommandToken& token : element.tokens) {
            if (token.offset > at) {
                out.append(element.text, at, token.offset - at);
                kept = true;
            }
            at = token.offset + token.text.size();
            switch (token.kind) {
            case CommandTokenKind::HomeFolder:
                out += homeText(m_context);
                kept = true;
                break;
            case CommandTokenKind::Variable:
                out += variable(token);
                kept = true;
                break;
            case CommandTokenKind::Flag:
                flag(token); // ES-DE removes a flag wherever it is
                break;
            case CommandTokenKind::Assignment:
                if (token.name == "INJECT") {
                    inject(element.text.substr(at));
                    return kept ? std::optional<std::string>(std::move(out)) : std::nullopt;
                }
                if (token.offset == 0) {
                    startDirElement(element);
                    return std::nullopt;
                }
                if (m_reported.insert(token.text).second) {
                    block(LaunchReadiness::RequiresConfiguration, "start-folder-glued",
                          fmt::format("the command's %STARTDIR%= is inside the argument {}, which ES-DE cuts apart: "
                                      "Retroverse does not",
                                      inQuotes(element.text)));
                }
                out += token.text;
                kept = true;
                break;
            case CommandTokenKind::Emulator:
                if (m_reported.insert("%EMULATOR_%").second) {
                    block(LaunchReadiness::RequiresConfiguration, "program-repeated",
                          fmt::format("the command names a second program, {}, which ES-DE does not expand",
                                      token.text));
                }
                out += token.text;
                kept = true;
                break;
            case CommandTokenKind::Core:
                out += core(token, element.text.substr(at));
                return out;
            case CommandTokenKind::PreCommand:
                unsupported(token, " (a program ES-DE runs before the command)");
                out += token.text;
                kept = true;
                break;
            case CommandTokenKind::Malformed:
                if (m_reported.insert(token.text).second) {
                    block(LaunchReadiness::RequiresConfiguration, "token-malformed",
                          fmt::format("the command's {} has no '=': ES-DE refuses to launch it", token.text));
                }
                out += token.text;
                kept = true;
                break;
            case CommandTokenKind::Unknown:
                unsupported(token, ", which ES-DE passes on as written");
                out += token.text;
                kept = true;
                break;
            }
        }
        if (at < element.text.size()) {
            out.append(element.text, at, std::string::npos);
            kept = true;
        }
        if (!kept && !element.text.empty()) {
            return std::nullopt; // flags alone
        }
        return out;
    }

    /// %CORE_X%: the core file is what follows the token and the one character after it (its separator), to the end of
    /// the argument; the token and the file become the core's path.
    std::string core(const CommandToken& token, std::string_view rest) {
        const std::string literal = token.text + std::string(rest);
        if (m_coreSeen) {
            if (m_reported.insert("%CORE_%").second) {
                block(LaunchReadiness::RequiresConfiguration, "core-repeated",
                      fmt::format("the command names a second core, {}, which ES-DE does not expand", token.text));
            }
            return literal;
        }
        m_coreSeen = true;
        if (rest.size() < 2) {
            block(LaunchReadiness::RequiresConfiguration, "core-malformed",
                  fmt::format("the command's {} names no core file", token.text));
            return literal;
        }
        // ES-DE looks for the core before it replaces the game's tokens: only '~' (replaced in the whole command
        // first) is expanded in the file's name.
        const std::string_view written = rest.substr(1);
        const std::vector<CommandToken> tokens = scanCommandTokens(written);
        if (std::any_of(tokens.begin(), tokens.end(),
                        [](const CommandToken& inner) { return inner.kind != CommandTokenKind::HomeFolder; })) {
            block(LaunchReadiness::RequiresConfiguration, "core-malformed",
                  fmt::format("the core file {} of {} holds a token", inQuotes(written), token.text));
            return literal;
        }
        const std::string coreFile = esdeReplace(std::string(written), "~", homeText(m_context));
        // What ES-DE refuses before it looks for the core: no corepath rule ("missing core configuration"), then a core
        // file that does not end ("invalid entry in systems configuration file").
        if (coreRules(m_context, token.target) == nullptr) {
            block(LaunchReadiness::RequiresConfiguration, "find-rules-missing",
                  fmt::format("no corepath rule defines the core {} (es_find_rules.xml)", token.target));
            return literal;
        }
        if (!coreFileEnds()) {
            block(LaunchReadiness::RequiresConfiguration, "core-malformed",
                  fmt::format("the command's {} ends the command, with no space after its file: ES-DE refuses it as "
                              "an invalid entry",
                              token.text));
            return literal;
        }
        if (!m_programFolder) {
            return literal; // no program: the core cannot be looked for
        }
        FindResult found = findCore(m_context, token.target, *m_programFolder, coreFile);
        m_translation.attempts.insert(m_translation.attempts.end(), found.attempts.begin(), found.attempts.end());
        if (found.undecided) {
            block(LaunchReadiness::RequiresConfiguration, "find-rule-undecided", *found.undecided);
            return literal;
        }
        if (!found.found) {
            block(LaunchReadiness::CoreNotFound, "core-not-found",
                  fmt::format("the core {} of {} is not on this machine", coreFile, token.target));
            return literal;
        }
        m_translation.core = std::move(found.found);
        return found.text;
    }

    /// Whether ES-DE finds where the core file ends (FileData::launchGame): at the next '"' after the %CORE_X% token
    /// when a quote opens the token, else at the next space, in the command as it stands then ('~' replaced; the flags
    /// it removed leave their spaces).
    [[nodiscard]] bool coreFileEnds() const {
        const std::string command = esdeReplace(m_command.text, "~", homeText(m_context));
        const std::size_t entry = command.find("%CORE_");
        const std::size_t close = entry == std::string::npos ? std::string::npos : command.find('%', entry + 6);
        if (close == std::string::npos) {
            return false;
        }
        const bool quoted = command.find("\"%CORE_") != std::string::npos;
        return (quoted && command.find('"', close) != std::string::npos) ||
               command.find(' ', close) != std::string::npos;
    }

    /// %INJECT%=<value>: dropped when ES-DE would inject nothing, else "requires launcher configuration".
    void inject(std::string_view value) {
        if (!m_values) {
            if (m_game == nullptr && m_reported.insert("%INJECT%=").second) {
                m_translation.notes.push_back(
                    fmt::format("the %INJECT% file {} is read for each game: a game that has one requires launcher "
                                "configuration",
                                inQuotes(value)));
            }
            return;
        }
        // ES-DE: '~' already replaced in the whole command, then %BASENAME%; a value of exactly %ROM% is the game's
        // path (romRaw: a folder game's folder); '\' turned into '/'; a value without a drive ("X:/") lies in the
        // folder holding %ROM%.
        std::string file = esdeReplace(std::string(value), "~", homeText(m_context));
        file = esdeReplace(std::move(file), "%BASENAME%", m_values->baseName);
        if (file == "%ROM%") {
            file = m_values->romRaw;
        }
        std::replace(file.begin(), file.end(), '\\', '/');
        const bool absolute = file.size() >= 3 && file[1] == ':' && file[2] == '/';
        if (!absolute) {
            file = m_values->gameDirGeneric + "/" + file;
        }
        file = preferred(std::move(file));
        const auto path = folderFromEsdeText(file);
        const InjectContent content = path ? injectContent(*path) : InjectContent::None;
        switch (content) {
        case InjectContent::None:
            m_translation.notes.push_back(
                fmt::format("%INJECT%: {} does not exist, so ES-DE injects nothing: the element is left out", file));
            break;
        case InjectContent::Empty:
            m_translation.notes.push_back(
                fmt::format("%INJECT%: {} is empty, so ES-DE injects nothing: the element is left out", file));
            break;
        case InjectContent::TooLong:
            m_translation.notes.push_back(fmt::format(
                "%INJECT%: {} holds more than {} bytes, so ES-DE injects nothing: the element is left out", file,
                kMaxInjectBytes));
            break;
        case InjectContent::Injected:
            block(LaunchReadiness::RequiresConfiguration, "inject-file",
                  fmt::format("ES-DE would inject the arguments of {} (%INJECT%), which Retroverse does not: choose "
                              "another command or an emulator profile for {}",
                              file, m_system.name.str()));
            break;
        case InjectContent::Unreadable:
            block(LaunchReadiness::RequiresConfiguration, "inject-file",
                  fmt::format("the %INJECT% file {} cannot be read", file));
            break;
        }
    }

    /// The working folder of %STARTDIR%=<value>: '~', %EMUDIR% and %GAMEDIR% replaced, as ES-DE replaces them there.
    std::filesystem::path startFolder(const CommandElement& element) {
        std::string out;
        std::size_t at = kStartDirToken.size();
        for (const CommandToken& token : element.tokens) {
            if (token.offset < kStartDirToken.size()) {
                continue; // the %STARTDIR%= itself
            }
            out.append(element.text, at, token.offset - at);
            at = token.offset + token.text.size();
            if (token.kind == CommandTokenKind::HomeFolder) {
                out += homeText(m_context);
            } else if (token.kind == CommandTokenKind::Variable && token.name == "EMUDIR") {
                if (!m_programFolder) {
                    return {};
                }
                out += folderText(*m_programFolder);
            } else if (token.kind == CommandTokenKind::Variable && token.name == "GAMEDIR") {
                if (!m_values) {
                    if (m_game == nullptr) {
                        m_translation.notes.push_back("the working folder is the game's folder (%STARTDIR%=%GAMEDIR%)");
                    }
                    return {};
                }
                out += m_values->gameDir;
            } else {
                unsupported(token, " in %STARTDIR%");
                out += token.text;
            }
        }
        out.append(element.text, std::min(at, element.text.size()), std::string::npos);
        if (out.size() < 2) {
            block(LaunchReadiness::RequiresConfiguration, "token-malformed",
                  "the command's %STARTDIR%= names no folder: ES-DE refuses to launch it");
            return {};
        }
        auto folder = folderFromEsdeText(out);
        if (!folder) {
            block(LaunchReadiness::RequiresConfiguration, "start-folder-relative",
                  fmt::format("the working folder {} (%STARTDIR%) is not an absolute folder: ES-DE resolves it "
                              "against its own working folder",
                              inQuotes(out)));
            return {};
        }
        // Checked for a system too: a folder reached here holds no %GAMEDIR% (a system returned above), so every game
        // of the system shares it.
        std::error_code error;
        const std::filesystem::path extended(core::extendedLengthPath(*folder));
        if (!std::filesystem::is_directory(extended, error)) {
            m_startFolderMissing = true;
            block(LaunchReadiness::RequiresConfiguration, "start-folder-missing",
                  fmt::format("the working folder {} (%STARTDIR%) does not exist: ES-DE creates it, Retroverse "
                              "writes nothing there",
                              core::pathToUtf8Lossy(*folder)));
        }
        return *folder;
    }

    /// The request (for a game), or the program's own check (for a system).
    void finish(const CommandElement& program, std::vector<std::string> arguments,
                std::filesystem::path workingDirectory) {
        launcher::LaunchRequest request;
        request.executable = m_translation.program->path;
        request.workingDirectory = std::move(workingDirectory);
        request.hideConsoleWindow = m_hideWindow;
        if (m_game == nullptr || !m_values) {
            // A system: the program alone must be one Windows starts (a PE .exe or .com).
            request.workingDirectory = request.executable.parent_path();
            if (const Status valid = launcher::validateLaunchRequest(request); !valid) {
                block(LaunchReadiness::RequiresConfiguration, "invalid-request", valid.error().toString());
            }
            return;
        }
        request.arguments = std::move(arguments);
        if (m_runInBackground) {
            request.waitStrategy = launcher::WaitStrategy::FollowWindow; // %RUNINBACKGROUND% (I11 step 13)
        }
        const std::string emulator = m_command.label.empty() ? (m_emulatorName.empty() ? program.text : m_emulatorName)
                                                             : m_command.label;
        request.label = fmt::format("{} with {}", m_values->title, emulator);
        if (const Status valid = launcher::validateLaunchRequest(request); !valid) {
            const bool folderReported =
                m_startFolderMissing && valid.error().context.starts_with("working directory");
            if (!folderReported) {
                block(LaunchReadiness::RequiresConfiguration, "invalid-request", valid.error().toString());
            }
        }
        m_translation.request = std::move(request);
    }

    const TranslationContext& m_context;
    const SystemDefinition& m_system;
    const SystemCommand& m_command;
    const catalog::LaunchGame* m_game = nullptr;
    LaunchTranslation m_translation;
    std::optional<GameValues> m_values;
    std::optional<CommandElement> m_startDir;
    std::optional<std::filesystem::path> m_programFolder;
    std::string m_emulatorName;
    /// What was reported once already (a token's text, or a pseudo-key).
    std::set<std::string> m_reported;
    bool m_hideWindow = false;
    bool m_coreSeen = false;
    bool m_startFolderMissing = false;
    /// I11 step 13: "open the game's file", %RUNINBACKGROUND%, and the %ESCAPESPECIALS% left to report.
    bool m_opensFile = false;
    bool m_runInBackground = false;
    std::optional<CommandToken> m_escapeSpecials;
};

} // namespace

FindResult findEmulator(const TranslationContext& context, std::string_view name) {
    FindResult result;
    if (context.programChoices != nullptr) {
        // Honoured or not found, never passed over: the find rules could start another emulator than the one chosen.
        if (const auto chosen = context.programChoices->find(name); chosen != context.programChoices->end()) {
            if (context.probe->fileExists(chosen->second)) {
                result.found = catalog::FoundLocation{chosen->second, "retroverse", std::string(name), {}};
            } else {
                result.attempts.push_back(fmt::format("the program chosen in Retroverse, {}, does not exist",
                                                      core::pathToUtf8Lossy(chosen->second)));
            }
            return result;
        }
    }
    const FindRuleSet* rules = emulatorRules(context, name);
    if (rules == nullptr) {
        result.noRules = true;
        return result;
    }
    RuleSearch search(context, result);
    for (const std::string& entry : rules->entries(FindRuleType::WinRegistryPath)) {
        if (search.appPath(entry)) {
            return result;
        }
    }
    for (const std::string& entry : rules->entries(FindRuleType::WinRegistryValue)) {
        if (search.registryValue(entry)) {
            return result;
        }
    }
    for (const std::string& entry : rules->entries(FindRuleType::SystemPath)) {
        if (search.systemPath(entry)) {
            return result;
        }
    }
    for (const std::string& entry : rules->entries(FindRuleType::StaticPath)) {
        if (search.staticPath(entry)) {
            return result;
        }
    }
    return result;
}

FindResult findCore(const TranslationContext& context, std::string_view name,
                    const std::filesystem::path& programFolder, std::string_view coreFile) {
    FindResult result;
    const FindRuleSet* rules = coreRules(context, name);
    if (rules == nullptr) {
        result.noRules = true;
        return result;
    }
    const std::string emulatorFolder = folderText(programFolder);
    for (const std::string& entry : rules->entries(FindRuleType::CorePath)) {
        // ES-DE: <entry>\<file> with '~' replaced; then the first %EMUPATH% (the program's folder) and the first
        // %ESPATH%, each followed by every '/' of the text turned into '\'.
        std::string text = esdeReplace(entry + "\\" + std::string(coreFile), "~", homeText(context));
        if (text.find("%EMUPATH%") != std::string::npos) {
            text = preferred(replaceFirst(std::move(text), "%EMUPATH%", emulatorFolder));
        }
        if (text.find("%ESPATH%") != std::string::npos) {
            if (!context.profile.installFolder) {
                result.undecided = fmt::format("corepath {} uses %ESPATH%, ES-DE's install folder, which is not known",
                                               inQuotes(entry));
                return result;
            }
            text = preferred(replaceFirst(std::move(text), "%ESPATH%", folderText(*context.profile.installFolder)));
        }
        const auto file = folderFromEsdeText(text);
        if (!file) {
            result.attempts.push_back(fmt::format("corepath {}: {} is not an absolute path", inQuotes(entry), inQuotes(text)));
            continue;
        }
        if (context.probe->fileExists(*file)) {
            result.found = FoundLocation{*file, std::string(toString(FindRuleType::CorePath)), entry, {}};
            // A path holding a space goes through getEscapedPath: quoted (one argument), in its generic form with '\'.
            result.text = text.find(' ') != std::string::npos ? preferred(genericText(text)) : std::move(text);
            return result;
        }
        result.attempts.push_back(
            fmt::format("corepath {}: {} does not exist", inQuotes(entry), core::pathToUtf8Lossy(*file)));
    }
    return result;
}

catalog::LaunchTranslation translateCommand(const TranslationContext& context, const SystemDefinition& system,
                                            const SystemCommand& command, const catalog::LaunchGame* game) {
    return CommandTranslation(context, system, command, game).run();
}

EsdeCommandTranslator::EsdeCommandTranslator(TranslationContext context, const SystemsConfiguration& systems)
    : m_context(std::move(context)), m_systems(&systems) {}

std::optional<std::vector<std::string>> EsdeCommandTranslator::commandLabels(const catalog::SystemName& system) const {
    const SystemDefinition* definition = m_systems->find(system.str());
    if (definition == nullptr) {
        return std::nullopt;
    }
    std::vector<std::string> labels;
    labels.reserve(definition->commands.size());
    for (const SystemCommand& command : definition->commands) {
        labels.push_back(command.label);
    }
    return labels;
}

catalog::LaunchTranslation EsdeCommandTranslator::translate(const catalog::SystemName& system, std::string_view label,
                                                            const catalog::LaunchGame* game) const {
    const SystemDefinition* definition = m_systems->find(system.str());
    const SystemCommand* command = definition != nullptr ? definition->findCommand(label) : nullptr;
    if (command == nullptr) {
        catalog::LaunchTranslation translation;
        translation.blockers.push_back(ReadinessBlocker{
            LaunchReadiness::RequiresConfiguration, "choice-stale",
            fmt::format("{} has no command labelled {}", system.str(), inQuotes(label))});
        return translation;
    }
    return translateCommand(m_context, *definition, *command, game);
}

Status checkProgramChoice(const std::filesystem::path& program) {
    const std::string context = core::pathToUtf8Lossy(program);
    if (!program.is_absolute()) {
        return makeError(ErrorCode::InvalidArgument, "a program's path must be absolute", context);
    }
    std::error_code ignored;
    if (!std::filesystem::is_regular_file(std::filesystem::path(core::extendedLengthPath(program)), ignored)) {
        return makeError(ErrorCode::NotFound, "no such program", context);
    }
    if (const auto refused = refusedProgram(program)) {
        return makeError(ErrorCode::ValidationFailed,
                         fmt::format("{} is a command interpreter or a launcher, which Retroverse never starts", *refused),
                         context);
    }
    launcher::LaunchRequest request;
    request.executable = program;
    request.workingDirectory = program.parent_path();
    return launcher::validateLaunchRequest(request);
}

Expected<ListedPath> listedPath(const std::filesystem::path& romFolder, std::string_view relativePath) {
    if (!core::isValidUtf8(relativePath)) {
        return makeError(ErrorCode::InvalidArgument, "the path is not valid UTF-8 text");
    }
    // Every element checked before any is looked up: FindFirstFileExW reads '*', '?', '<', '>' and '"' as wildcards.
    std::vector<std::string_view> elements;
    std::size_t start = 0;
    while (start <= relativePath.size()) {
        std::size_t end = relativePath.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = relativePath.size();
        }
        const std::string_view element = relativePath.substr(start, end - start);
        const bool forbidden = std::any_of(element.begin(), element.end(), [](char c) {
            return static_cast<unsigned char>(c) < 0x20 || std::string_view("*?<>\"|:").find(c) != std::string_view::npos;
        });
        if (element.empty() || element == "." || element == ".." || forbidden) {
            return makeError(ErrorCode::InvalidArgument, "not a path below the ROM folder",
                             std::string(relativePath));
        }
        elements.push_back(element);
        start = end + 1;
    }
    ListedPath listed;
    for (const std::string_view element : elements) {
        const std::string candidate =
            listed.relativePath.empty() ? std::string(element) : listed.relativePath + "/" + std::string(element);
        auto full = core::extendedLengthPathBelow(romFolder, candidate);
        if (!full) {
            return propagate(full.error());
        }
        WIN32_FIND_DATAW data{};
        const HANDLE handle =
            FindFirstFileExW(full->c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            return makeError(core::fileErrorCode(error, romFolder), core::systemErrorMessage(error), candidate);
        }
        FindClose(handle);
        auto name = core::wideToUtf8(data.cFileName);
        if (!name) {
            return makeError(ErrorCode::IoError, "a name on disk is not valid UTF-16", candidate);
        }
        listed.relativePath =
            listed.relativePath.empty() ? std::move(*name) : listed.relativePath + "/" + std::move(*name);
        listed.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return listed;
}

} // namespace rv::esde
