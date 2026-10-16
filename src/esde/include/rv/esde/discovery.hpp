#pragma once

// Where an ES-DE installation keeps its files (ADR-0008 §6, §8), found the way ES-DE 3.4.1 finds them, each value
// with its source:
//  - the install folder (ES-DE.exe, resources/): given explicitly (library.json, --install), else the ES-DE uninstall
//    entry in the registry (DisplayName "ES-DE" or "ES-DE <version>", its InstallLocation or DisplayIcon), whose
//    DisplayVersion is the version;
//  - the ES-DE home (settings/, gamelists/, custom_systems/): given explicitly (library.json's ES-DE home, or ES-DE's
//    own --home <folder>, whose ES-DE home is <folder>\ES-DE), else portable (a portable.txt beside ES-DE.exe: its
//    first line, empty or a path relative to the install folder, names ES-DE's home folder, and the ES-DE home is its
//    ES-DE folder), else the user's home folder's ES-DE folder.
// ES-DE's home folder, which '~' stands for in its settings, find rules and commands, is always the ES-DE home's
// parent. Also here: where ES-DE finds a bundled resource, whether a folder can be used, and library.json, the library
// profile under <user data>/config. Read-only: nothing is created or written but library.json by writeLibraryFile().

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/system_probe.hpp>

namespace rv::esde {

/// Where the ES-DE home came from.
enum class HomeSource : std::uint8_t {
    None,     ///< no home folder is known (no explicit value, no portable.txt, no HOMEDRIVE/HOMEPATH or USERPROFILE)
    Explicit, ///< library.json, or a --home option
    Portable, ///< portable.txt beside ES-DE.exe
    Default,  ///< the user's home folder (ISystemProbe::homeFolder()) and its ES-DE folder
};

/// "none", "explicit", "portable", "default".
[[nodiscard]] std::string_view toString(HomeSource source) noexcept;

/// Where the install folder came from.
enum class InstallSource : std::uint8_t {
    None,           ///< not found: no bundled systems or find rules (but those the ES-DE home's resources/ holds), and
                    ///< %ESPATH% unresolved
    Explicit,       ///< library.json, or an --install option
    UninstallEntry, ///< the ES-DE uninstall entry in the registry
};

/// "none", "explicit", "uninstall-entry".
[[nodiscard]] std::string_view toString(InstallSource source) noexcept;

/// What the caller knows already; empty values are discovered.
struct DiscoveryOptions {
    /// The ES-DE home itself (library.json's): the folder holding settings/ and gamelists/.
    std::optional<std::filesystem::path> esdeHome;
    /// ES-DE's home folder as ES-DE's own --home option takes it: the ES-DE home is its ES-DE folder. Not with
    /// esdeHome.
    std::optional<std::filesystem::path> homeFolder;
    /// The install folder (library.json's, or --install).
    std::optional<std::filesystem::path> installFolder;
};

/// What discovery found.
struct EsdeDiscovery {
    /// The ES-DE home; nothing when no home folder is known.
    std::optional<std::filesystem::path> esdeHome;
    HomeSource homeSource = HomeSource::None;
    /// The ES-DE home is an existing folder.
    bool esdeHomeExists = false;
    std::optional<std::filesystem::path> installFolder;
    InstallSource installSource = InstallSource::None;
    /// The uninstall entry found, its hive and key ("HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\
    /// Uninstall\ES-DE"), whether it gave the install folder or only the version; empty when none was found.
    std::string uninstallKey;
    /// The DisplayVersion of the uninstall entry, when that entry describes the install folder ("3.4.1"); empty when
    /// not known.
    std::string version;
    /// ES-DE.exe is in the install folder.
    bool executableFound = false;
    /// The portable.txt beside ES-DE.exe, when there is one (whether or not it gave the home).
    std::optional<std::filesystem::path> portableFile;
    /// "home-unknown" (error), "home-missing" (error: the ES-DE home does not exist), "install-not-found" (warning),
    /// "install-incomplete" (warning: no ES-DE.exe in the install folder), "portable-home-invalid" (warning: ES-DE
    /// ignores a portable.txt whose home folder does not exist or is a file), "portable-unreadable" (warning).
    std::vector<catalog::Issue> issues;

    /// ES-DE's home folder, which '~' stands for: the ES-DE home's parent; nothing without an ES-DE home.
    [[nodiscard]] std::optional<std::filesystem::path> homeFolder() const;

    bool operator==(const EsdeDiscovery&) const = default;
};

/// Discovers the ES-DE installation: the machine through `probe` (the uninstall entry in the registry, the user's home
/// folder), ES-DE's own files through the file system (ES-DE.exe, portable.txt and its content, whether the ES-DE home
/// exists). Explicit values win; the uninstall entry is read even then, for the version. Nothing is fatal but the
/// arguments. Errors (InvalidArgument): a relative explicit path, or both esdeHome and homeFolder.
[[nodiscard]] Expected<EsdeDiscovery> discoverEsde(const DiscoveryOptions& options, const core::ISystemProbe& probe);

/// The library profile of a discovery: `id`, the ES-DE adapter, its ES-DE home and install folder, no overrides.
/// Errors (NotFound): no ES-DE home was found.
[[nodiscard]] Expected<catalog::LibraryProfile> profileOf(const EsdeDiscovery& discovery, catalog::LibraryId id);

/// Where ES-DE finds its bundled resource `relative` ("systems/windows/es_systems.xml"), as its ResourceManager does:
/// <ES-DE home>/resources/<relative> when that file exists, else <install folder>/resources/<relative> when that one
/// does; nothing when neither does (folders that are not known, or relative, are not looked in).
[[nodiscard]] std::optional<std::filesystem::path> bundledResource(const catalog::LibraryProfile& profile,
                                                                   std::string_view relative);

/// Whether a folder is there, as the catalog reports a source folder (ADR-0008 §4). Whether it can be listed is the
/// ROM scan's to find out.
struct FolderCheck {
    catalog::Availability availability = catalog::Availability::Available;
    /// None for Available; FolderMissing (no such folder, or a file of that name), Unreachable (its drive or share
    /// cannot be reached), NotListable (anything else: its attributes cannot be read, access denied, ...).
    catalog::SourceUnavailableReason reason = catalog::SourceUnavailableReason::None;
    /// What the file system said, for people; empty for Available.
    std::string detail;

    bool operator==(const FolderCheck&) const = default;
};

/// Checks that `folder` exists and is a folder (its attributes, through its \\?\ form: any length). Never throws.
[[nodiscard]] FolderCheck checkFolder(const std::filesystem::path& folder);

/// library.json's file name; it lives in <user data>/config (core::Paths::configDir()).
inline constexpr std::string_view kLibraryFileName = "library.json";
/// The version of library.json's format ("schemaVersion").
inline constexpr int kLibraryFileSchemaVersion = 1;

/// Reads a library.json (JSON with comments): "schemaVersion" 1, "id" (a LibraryId), "adapter" ("esde"), "esdeHome"
/// and, optional or null, "esdeInstall", "romRoot" and "mediaRoot"; folders absolute, UTF-8. Errors: those of reading
/// it (NotFound, ...), ParseError (not JSON, or not an object), InvalidArgument naming the key (a missing or unknown
/// key, a value of the wrong kind, an invalid id, an unknown adapter, a relative or empty folder), Unsupported (another
/// schemaVersion).
[[nodiscard]] Expected<catalog::LibraryProfile> readLibraryFile(const std::filesystem::path& file);

/// The text of library.json for `profile`: a comment saying what it is, then the keys readLibraryFile() reads, the
/// optional ones null when not set.
[[nodiscard]] std::string libraryFileText(const catalog::LibraryProfile& profile);

/// Whether writeLibraryFile() may replace a library.json that is there.
enum class ExistingFile : std::uint8_t {
    Keep,    ///< never replaced, also when it appears while the new one is written
    Replace, ///< replaced
};

/// Writes libraryFileText() to `file` atomically (its folder created when missing; readers never see part of it). With
/// ExistingFile::Keep the new file is written beside it and moved into place by a rename that replaces nothing, so a
/// library.json there, or one created meanwhile (another `library create`), is kept: Conflict. Other errors: IoError
/// and those of core::writeTextFileAtomic().
[[nodiscard]] Status writeLibraryFile(const std::filesystem::path& file, const catalog::LibraryProfile& profile,
                                      ExistingFile existing);

} // namespace rv::esde
