#include <rv/esde/discovery.hpp>

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/esde/settings.hpp>

namespace rv::esde {
namespace {

/// The ES-DE home's name in ES-DE's home folder (Utils::FileSystem::getAppDataDirectory).
constexpr std::wstring_view kEsdeFolderName = L"ES-DE";
constexpr std::string_view kUninstallDisplayName = "ES-DE";
constexpr std::wstring_view kExecutableName = L"ES-DE.exe";
constexpr std::wstring_view kPortableFileName = L"portable.txt";
/// More than any portable.txt holds (one line naming a folder).
constexpr std::uintmax_t kMaxPortableBytes = 64ull * 1024ull;
/// More than any library.json holds.
constexpr std::uintmax_t kMaxLibraryFileBytes = 1024ull * 1024ull;

catalog::Issue issueOf(catalog::IssueSeverity severity, std::string code, std::string file, std::string reason) {
    return catalog::Issue{severity, std::move(code), {}, std::move(file), std::move(reason)};
}

/// The same folder or file, compared as the file system compares names.
bool samePlace(const std::filesystem::path& a, const std::filesystem::path& b) {
    return core::pathIsWithin(a, b) && core::pathIsWithin(b, a);
}

/// An absolute folder made tidy for keeping and showing (folderFromEsdeText()): "." and ".." resolved lexically (as
/// Windows resolves them), '\' between the elements, no repeated or trailing separator (but a drive root's). Nothing
/// when it is not absolute.
std::optional<std::filesystem::path> tidyFolder(const std::filesystem::path& folder) {
    if (folder.empty() || !folder.is_absolute()) {
        return std::nullopt;
    }
    return folderFromEsdeText(core::pathToUtf8Lossy(folder));
}

/// True when `path` names an existing file (not a folder), whatever its length.
bool isFile(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(core::extendedLengthPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/// The first line of portable.txt as ES-DE reads it (std::getline on a stream opened in text mode): the text ends at a
/// Ctrl+Z, the line at the first line feed, a carriage return right before that line feed dropped (text mode reads
/// "\r\n" as "\n"). A byte order mark is kept, as ES-DE keeps it.
std::string_view firstLineOf(std::string_view text) {
    text = text.substr(0, text.find('\x1A'));
    const std::size_t end = text.find('\n');
    if (end == std::string_view::npos) {
        return text;
    }
    std::string_view line = text.substr(0, end);
    if (line.ends_with('\r')) {
        line.remove_suffix(1);
    }
    return line;
}

/// The home folder portable.txt names (ES-DE's parseArguments): its first line appended to the install folder, or the
/// install folder for an empty line; nothing (with the issue) when it cannot be read or ES-DE would reject it.
std::optional<std::filesystem::path> portableHome(const std::filesystem::path& installFolder,
                                                  const std::filesystem::path& portableFile,
                                                  std::vector<catalog::Issue>& issues) {
    const std::string file = core::pathToUtf8Lossy(portableFile);
    const auto content = core::readFileShared(portableFile, kMaxPortableBytes);
    if (!content) {
        issues.push_back(issueOf(catalog::IssueSeverity::Warning, "portable-unreadable", file,
                                 fmt::format("portable.txt cannot be read ({}): the default home is used",
                                             content.error().message)));
        return std::nullopt;
    }
    const std::string_view line = firstLineOf(*content);
    std::optional<std::filesystem::path> home;
    if (line.empty()) {
        home = installFolder;
    } else if (core::isValidUtf8(line)) {
        // ES-DE appends the line whatever it holds ("<install>/<line>"): an absolute path there names no folder.
        home = folderFromEsdeText(esdeGenericText(installFolder) + "/" + std::string(line));
    }
    if (!home || checkFolder(*home).availability != catalog::Availability::Available) {
        issues.push_back(issueOf(
            catalog::IssueSeverity::Warning, "portable-home-invalid", file,
            fmt::format("portable.txt names the home folder \"{}\", which is no existing folder: ES-DE ignores it and "
                        "uses the default home",
                        core::isValidUtf8(line) ? std::string(line) : core::windows1252ToUtf8(line))));
        return std::nullopt;
    }
    return home;
}

/// A folder of library.json: a non-empty string naming an absolute folder.
Expected<std::filesystem::path> libraryFolder(const nlohmann::json& value, std::string_view key,
                                              const std::string& context) {
    if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("\"{}\" must be a folder (a non-empty string)", key),
                         context);
    }
    const std::string& text = value.get_ref<const std::string&>();
    auto path = core::tryPathFromUtf8(text);
    if (!path || !path->is_absolute()) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("\"{}\" must be an absolute folder", key), context);
    }
    auto folder = tidyFolder(*path);
    if (!folder) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("\"{}\" must be an absolute folder", key), context);
    }
    return std::move(*folder);
}

nlohmann::ordered_json folderJson(const std::optional<std::filesystem::path>& folder) {
    if (!folder) {
        return nullptr;
    }
    return core::pathToUtf8Lossy(*folder);
}

} // namespace

std::string_view toString(HomeSource source) noexcept {
    switch (source) {
    case HomeSource::None:
        return "none";
    case HomeSource::Explicit:
        return "explicit";
    case HomeSource::Portable:
        return "portable";
    case HomeSource::Default:
        return "default";
    }
    return "none";
}

std::string_view toString(InstallSource source) noexcept {
    switch (source) {
    case InstallSource::None:
        return "none";
    case InstallSource::Explicit:
        return "explicit";
    case InstallSource::UninstallEntry:
        return "uninstall-entry";
    }
    return "none";
}

std::optional<std::filesystem::path> EsdeDiscovery::homeFolder() const {
    if (!esdeHome) {
        return std::nullopt;
    }
    return esdeHome->parent_path();
}

Expected<EsdeDiscovery> discoverEsde(const DiscoveryOptions& options, const core::ISystemProbe& probe) {
    if (options.esdeHome && options.homeFolder) {
        return makeError(ErrorCode::InvalidArgument,
                         "give either the ES-DE home or ES-DE's home folder (--home), not both", "homeFolder");
    }
    const auto explicitFolder = [](const std::optional<std::filesystem::path>& value,
                                   std::string_view name) -> Expected<std::optional<std::filesystem::path>> {
        if (!value) {
            return std::optional<std::filesystem::path>{};
        }
        auto folder = tidyFolder(*value);
        if (!folder) {
            return makeError(ErrorCode::InvalidArgument, "must be an absolute folder",
                             fmt::format("{}={}", name, core::pathToUtf8Lossy(*value)));
        }
        return folder;
    };
    auto esdeHome = explicitFolder(options.esdeHome, "esdeHome");
    auto homeFolder = explicitFolder(options.homeFolder, "homeFolder");
    auto installFolder = explicitFolder(options.installFolder, "installFolder");
    for (const auto* checked : {&esdeHome, &homeFolder, &installFolder}) {
        if (!*checked) {
            return propagate(checked->error());
        }
    }

    EsdeDiscovery result;
    // The install folder: explicit, else the uninstall entry; the version when the entry describes that folder.
    const auto entry = core::findUninstallEntry(probe, kUninstallDisplayName);
    const auto entryFolder = entry ? entry->installFolder() : std::nullopt;
    if (entry) {
        result.uninstallKey = fmt::format("{}\\{}", core::toString(entry->root), entry->keyPath);
    }
    if (*installFolder) {
        result.installFolder = **installFolder;
        result.installSource = InstallSource::Explicit;
    } else if (entryFolder) {
        result.installFolder = tidyFolder(*entryFolder);
        result.installSource = result.installFolder ? InstallSource::UninstallEntry : InstallSource::None;
    }
    if (result.installFolder) {
        if (entryFolder && samePlace(*entryFolder, *result.installFolder)) {
            result.version = entry->displayVersion;
        }
        result.executableFound = isFile(*result.installFolder / kExecutableName);
        if (!result.executableFound) {
            result.issues.push_back(issueOf(catalog::IssueSeverity::Warning, "install-incomplete",
                                            core::pathToUtf8Lossy(*result.installFolder),
                                            "the install folder holds no ES-DE.exe"));
        }
        if (const std::filesystem::path portable = *result.installFolder / kPortableFileName; isFile(portable)) {
            result.portableFile = portable;
        }
    } else {
        result.issues.push_back(issueOf(catalog::IssueSeverity::Warning, "install-not-found", {},
                                        "ES-DE's install folder was not found (no uninstall entry, none given): its "
                                        "bundled systems and find rules cannot be read"));
    }

    // The ES-DE home: explicit, else portable, else the user's home folder.
    if (*esdeHome) {
        result.esdeHome = **esdeHome;
        result.homeSource = HomeSource::Explicit;
    } else if (*homeFolder) {
        result.esdeHome = **homeFolder / kEsdeFolderName;
        result.homeSource = HomeSource::Explicit;
    } else {
        if (result.portableFile) {
            if (const auto home = portableHome(*result.installFolder, *result.portableFile, result.issues)) {
                result.esdeHome = *home / kEsdeFolderName;
                result.homeSource = HomeSource::Portable;
            }
        }
        if (const auto home = probe.homeFolder(); !result.esdeHome && home) {
            if (const auto folder = tidyFolder(*home)) {
                result.esdeHome = *folder / kEsdeFolderName;
                result.homeSource = HomeSource::Default;
            }
        }
    }
    if (!result.esdeHome) {
        result.issues.push_back(issueOf(catalog::IssueSeverity::Error, "home-unknown", {},
                                        "no home folder is known (no HOMEDRIVE and HOMEPATH, no USERPROFILE): give "
                                        "the ES-DE home"));
        return result;
    }
    const FolderCheck home = checkFolder(*result.esdeHome);
    result.esdeHomeExists = home.availability == catalog::Availability::Available;
    if (!result.esdeHomeExists) {
        // A --home naming the ES-DE home itself is the likely mistake: say so.
        const bool homeItself = *homeFolder && checkFolder(**homeFolder / L"settings").availability ==
                                                   catalog::Availability::Available;
        result.issues.push_back(issueOf(
            catalog::IssueSeverity::Error, "home-missing", core::pathToUtf8Lossy(*result.esdeHome),
            fmt::format("the ES-DE home is no folder: {}{}", home.detail,
                        homeItself ? " (the folder given looks like the ES-DE home itself: --home names the folder "
                                     "that holds it, as ES-DE's own --home does)"
                                   : "")));
    }
    return result;
}

Expected<catalog::LibraryProfile> profileOf(const EsdeDiscovery& discovery, catalog::LibraryId id) {
    if (!discovery.esdeHome) {
        return makeError(ErrorCode::NotFound, "no ES-DE home was found", "esdeHome");
    }
    catalog::LibraryProfile profile;
    profile.id = std::move(id);
    profile.adapter = catalog::LibraryAdapter::Esde;
    profile.esdeHome = *discovery.esdeHome;
    profile.installFolder = discovery.installFolder;
    return profile;
}

std::optional<std::filesystem::path> bundledResource(const catalog::LibraryProfile& profile,
                                                     std::string_view relative) {
    auto relativePath = core::tryPathFromUtf8(relative);
    if (!relativePath) {
        return std::nullopt;
    }
    std::vector<std::filesystem::path> candidates; // absolute folders only: never the current folder
    if (profile.esdeHome.is_absolute()) {
        candidates.push_back(profile.esdeHome / "resources" / *relativePath);
    }
    if (profile.installFolder && profile.installFolder->is_absolute()) {
        candidates.push_back(*profile.installFolder / "resources" / *relativePath);
    }
    for (std::filesystem::path& candidate : candidates) {
        candidate.make_preferred();
        // ES-DE takes the first that exists (Utils::FileSystem::exists: a file or a folder).
        if (GetFileAttributesW(core::extendedLengthPath(candidate).c_str()) != INVALID_FILE_ATTRIBUTES) {
            return std::move(candidate);
        }
    }
    return std::nullopt;
}

FolderCheck checkFolder(const std::filesystem::path& folder) {
    FolderCheck check;
    const DWORD attributes = GetFileAttributesW(core::extendedLengthPath(folder).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        check.availability = catalog::Availability::SourceUnavailable;
        switch (core::fileErrorCode(code, folder)) {
        case ErrorCode::NotFound:
            check.reason = catalog::SourceUnavailableReason::FolderMissing;
            break;
        case ErrorCode::Unavailable:
            check.reason = catalog::SourceUnavailableReason::Unreachable;
            break;
        default:
            check.reason = catalog::SourceUnavailableReason::NotListable;
            break;
        }
        check.detail = core::systemErrorMessage(code);
        return check;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        check.availability = catalog::Availability::SourceUnavailable;
        check.reason = catalog::SourceUnavailableReason::FolderMissing;
        check.detail = "a file, not a folder";
    }
    return check;
}

Expected<catalog::LibraryProfile> readLibraryFile(const std::filesystem::path& file) {
    const std::string context = core::pathToUtf8Lossy(file);
    const auto text = core::readTextFile(file, kMaxLibraryFileBytes);
    if (!text) {
        return propagate(text.error());
    }
    const auto json = core::parseJson(*text, context);
    if (!json) {
        return propagate(json.error());
    }
    if (!json->is_object()) {
        return makeError(ErrorCode::ParseError, "library.json must hold a JSON object", context);
    }
    static constexpr std::array<std::string_view, 7> kKeys{"schemaVersion", "id",      "adapter",  "esdeHome",
                                                           "esdeInstall",   "romRoot", "mediaRoot"};
    for (const auto& item : json->items()) {
        if (std::find(kKeys.begin(), kKeys.end(), item.key()) == kKeys.end()) {
            return makeError(ErrorCode::InvalidArgument, fmt::format("unknown key \"{}\"", item.key()), context);
        }
    }
    const auto required = [&](std::string_view key) -> const nlohmann::json* {
        const auto found = json->find(key);
        return found == json->end() || found->is_null() ? nullptr : &*found;
    };
    const nlohmann::json* version = required("schemaVersion");
    if (version == nullptr || !version->is_number_integer()) {
        return makeError(ErrorCode::InvalidArgument, "\"schemaVersion\" must be an integer", context);
    }
    if (version->get<long long>() != kLibraryFileSchemaVersion) {
        return makeError(ErrorCode::Unsupported,
                         fmt::format("schemaVersion {} is not supported (this version reads {})",
                                     version->get<long long>(), kLibraryFileSchemaVersion),
                         context);
    }
    catalog::LibraryProfile profile;
    const nlohmann::json* id = required("id");
    if (id == nullptr || !id->is_string()) {
        return makeError(ErrorCode::InvalidArgument, "\"id\" must be a string", context);
    }
    auto parsedId = catalog::LibraryId::parse(id->get_ref<const std::string&>());
    if (!parsedId) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("\"id\": {}", parsedId.error().message), context);
    }
    profile.id = std::move(*parsedId);
    const nlohmann::json* adapter = required("adapter");
    const auto parsedAdapter = adapter != nullptr && adapter->is_string()
                                   ? catalog::parseLibraryAdapter(adapter->get_ref<const std::string&>())
                                   : std::nullopt;
    if (!parsedAdapter) {
        return makeError(ErrorCode::InvalidArgument, "\"adapter\" must be \"esde\"", context);
    }
    profile.adapter = *parsedAdapter;
    const nlohmann::json* home = required("esdeHome");
    if (home == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "\"esdeHome\" is missing", context);
    }
    auto esdeHome = libraryFolder(*home, "esdeHome", context);
    if (!esdeHome) {
        return propagate(esdeHome.error());
    }
    profile.esdeHome = std::move(*esdeHome);
    for (const auto& [key, target] : {std::pair{std::string_view("esdeInstall"), &profile.installFolder},
                                      std::pair{std::string_view("romRoot"), &profile.romRoot},
                                      std::pair{std::string_view("mediaRoot"), &profile.mediaRoot}}) {
        if (const nlohmann::json* value = required(key)) {
            auto folder = libraryFolder(*value, key, context);
            if (!folder) {
                return propagate(folder.error());
            }
            *target = std::move(*folder);
        }
    }
    return profile;
}

std::string libraryFileText(const catalog::LibraryProfile& profile) {
    nlohmann::ordered_json json;
    json["schemaVersion"] = kLibraryFileSchemaVersion;
    json["id"] = profile.id.str();
    json["adapter"] = std::string(catalog::toString(profile.adapter));
    json["esdeHome"] = core::pathToUtf8Lossy(profile.esdeHome);
    json["esdeInstall"] = folderJson(profile.installFolder);
    json["romRoot"] = folderJson(profile.romRoot);
    json["mediaRoot"] = folderJson(profile.mediaRoot);
    return "// The library Retroverse reads (ADR-0008 §6), written by \"retroverse-cli library create\". JSON with\n"
           "// comments; folders are absolute. esdeHome is the ES-DE home (settings, gamelists); esdeInstall the\n"
           "// folder of ES-DE.exe; romRoot and mediaRoot, when set, replace ES-DE's ROMDirectory and MediaDirectory.\n"
           "// The catalog database is the authority on the id: changing it here does not rename the library.\n" +
           json.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

Status writeLibraryFile(const std::filesystem::path& file, const catalog::LibraryProfile& profile,
                        ExistingFile existing) {
    const std::string text = libraryFileText(profile);
    if (existing == ExistingFile::Replace) {
        return core::writeTextFileAtomic(file, text);
    }
    // Written whole under a name of this process's, then moved into place without MOVEFILE_REPLACE_EXISTING: the
    // rename fails rather than replace a library.json there, whenever it appeared.
    std::filesystem::path written = file;
    written += fmt::format(".new-{}", GetCurrentProcessId());
    if (const Status status = core::writeTextFileAtomic(written, text); !status) {
        return status;
    }
    if (MoveFileExW(core::extendedLengthPath(written).c_str(), core::extendedLengthPath(file).c_str(),
                    MOVEFILE_WRITE_THROUGH)) {
        return ok();
    }
    const DWORD error = GetLastError();
    std::error_code ignored;
    std::filesystem::remove(written, ignored);
    const std::string context = core::pathToUtf8Lossy(file);
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        return makeError(ErrorCode::Conflict, "the file exists already", context);
    }
    return makeError(ErrorCode::IoError, "cannot move the new file into place: " + core::systemErrorMessage(error),
                     context);
}

} // namespace rv::esde
