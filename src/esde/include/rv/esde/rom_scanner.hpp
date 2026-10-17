#pragma once

// A system's ROM folder listed with ES-DE 3.4.1's discovery rules (SystemData::populateFolder, ADR-0008 §3): what ES-DE
// would show of each file and folder before the gamelist is applied, and the facts the gamelist's rules need.
// Read-only: the folder is walked with core::walkTree (\\?\ paths, any length; links followed unless they loop), and no
// file is opened. The rules:
//  - a name's extension is what follows its last '.', the dot included, and "." when it has none or ends with one
//    (esdeExtension()); it is compared with the system's extensions verbatim and with case (".Sfc" is neither ".sfc"
//    nor ".SFC");
//  - a file with a configured extension is a game; so is a directory, which ES-DE does not enter then, unless its
//    extension is "." (a directory without one is never a game); every other directory is entered, at any depth;
//  - with ShowHiddenFiles off, a hidden file or folder (the hidden attribute, or a name with a leading dot:
//    Utils::FileSystem::isHidden) is skipped, and nothing below a hidden folder is seen;
//  - a folder holding a noload.txt (a file or folder of that name, in any case: Utils::FileSystem::exists) is not
//    entered: nothing below it is a game. A noload.txt in the system folder itself (named exactly so: ES-DE looks for
//    it in the folder's listing) keeps ES-DE from populating the system at all (RomFolderListing::noLoadAtRoot);
//  - a would-be game whose name ES-DE leaves empty is skipped "as it has no filename" (a file named ".gb": its stem is
//    empty); a would-be game directory whose name is empty is entered as a folder instead;
//  - in a system of the arcade or neogeo platform (isArcadePlatform()), a file whose stem is a MAME BIOS or device set
//    is skipped, and a directory that is one is entered as a folder;
//  - the name ES-DE gives a file without metadata: its stem, or in an arcade system its MAME name (mameName()); a
//    directory that is a game loses its extension.
// What is excluded keeps its place in the listing with the reason, so that the library source can say why a file the
// user sees on disk is not a game. Deliberate differences from ES-DE: a link loop is found by the identity of the
// folders (core::walkTree) rather than ES-DE's canonical-path and 20-level checks; folders that cannot be listed are
// reported (RomFolderListing::unavailableFolders), never taken as empty.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>
#include <rv/esde/mame_sets.hpp>

namespace rv::esde {

/// The codes of the issues scanRomFolder() reports (catalog::Issue::code), each naming the system and the file or
/// folder.
/// A folder below the system folder that cannot be listed (access denied, an I/O error, a link that loops or whose
/// target cannot be opened): its content is unknown, "source unavailable" (RomFolderListing::unavailableFolders).
/// Error.
inline constexpr std::string_view kRomIssueFolderUnavailable = "rom-folder-unavailable";
/// An entry that cannot be listed: a name without a UTF-8 form (an unpaired UTF-16 surrogate), a link to a file whose
/// target cannot be opened. It is left out. Warning.
inline constexpr std::string_view kRomIssueEntryUnreadable = "rom-entry-unreadable";
/// A name the catalog cannot key (a character the Windows file systems refuse, which another file system may hold):
/// left out. Warning.
inline constexpr std::string_view kRomIssueNameUnsupported = "rom-name-unsupported";
/// A file that would be a game but whose name ES-DE leaves empty: ES-DE skips it "as it has no filename" (a file named
/// ".gb", an arcade file whose name is all parentheses). Warning.
inline constexpr std::string_view kRomIssueNameEmpty = "rom-name-empty";

/// ES-DE's Utils::FileSystem::getExtension of a name: from its last '.' to its end, the dot included; "." when the name
/// has no '.' or ends with one. A name with a leading dot only is all extension (".gb").
[[nodiscard]] std::string_view esdeExtension(std::string_view name) noexcept;

/// ES-DE's Utils::FileSystem::getStem of a name: a file's name without its extension (up to its last '.'), a folder's
/// whole name.
[[nodiscard]] std::string_view esdeStem(std::string_view name, bool directory) noexcept;

/// What a file or folder is to ES-DE's naming.
enum class EsdeNameKind : std::uint8_t {
    File,          ///< a file
    GameDirectory, ///< a directory whose name carries a configured extension: a game, named without its extension
    Folder,        ///< a directory ES-DE enters: named with its whole name, never from mamenames.xml
};

/// What a system's scan follows.
struct ScanRules {
    /// The system's extensions, verbatim (SystemDefinition::extensions).
    std::vector<std::string> extensions;
    /// ES-DE's ShowHiddenFiles.
    bool showHiddenFiles = true;
    /// The system is of the arcade or neogeo platform (isArcadePlatform()): its BIOS and device sets are skipped, its
    /// games named from mamenames.xml.
    bool arcade = false;
    /// The MAME lists, for an arcade system; null reads as empty lists.
    const MameSets* mame = nullptr;
    /// ES-DE's MAMENameStripExtraInfo.
    bool mameNameStripExtraInfo = true;
    /// Read before each entry of the walk: once it holds true, the scan ends with Cancelled.
    const std::atomic<bool>* cancel = nullptr;
};

/// The name ES-DE gives `relativePath` (a path below the system folder, '/' between the elements) before its gamelist
/// does, as the catalog shows it (ScannedItem::defaultName, CatalogEntry::title): a file's stem, a game directory's
/// name without its extension, a folder's whole name (catalog::titleFromFileStem()); for a game of an arcade system its
/// MAME name (mameName(), a game directory's cut by its extension's length as ES-DE cuts it), cleaned for one line
/// (catalog::displayText()), the file's stem when nothing is left. Never empty. `esdeName`, when given, receives
/// ES-DE's own text of the name before the cut: empty when ES-DE skips a file "as it has no filename".
[[nodiscard]] std::string esdeDefaultName(std::string_view relativePath, EsdeNameKind kind, const ScanRules& rules,
                                          std::string* esdeName = nullptr);

/// What ES-DE's populateFolder makes of a listed file or folder.
enum class ScanVerdict : std::uint8_t {
    Game,          ///< listed as a game: a file, or a directory whose name carries a configured extension
    Excluded,      ///< it would be a game, but ScannedItem::exclusion keeps it out
    NameEmpty,     ///< a file that would be a game, skipped because its ES-DE name is empty (kRomIssueNameEmpty)
    NotConfigured, ///< a file whose extension the system does not configure
    Folder,        ///< a directory ES-DE enters (or would, but for ScannedItem::exclusion or ScannedItem::above)
};

/// Why a file or folder is kept out by populateFolder.
enum class ScanExclusion : std::uint8_t {
    None,
    Hidden,          ///< ShowHiddenFiles off: the item is hidden, or a folder above it is
    NoLoadTxt,       ///< a folder above it holds a noload.txt; for a folder, it holds one itself
    BiosOrDeviceSet, ///< a MAME BIOS or device set of an arcade system
};

/// A file or folder of the system's ROM folder.
struct ScannedItem {
    /// The path below the system folder as listed, and its key.
    catalog::LookupKey key;
    /// Directory, size, last write time and hidden (the attribute or a leading dot), as listed.
    catalog::FileFacts facts;
    ScanVerdict verdict = ScanVerdict::NotConfigured;
    /// For Excluded: why. For a Folder: Hidden or NoLoadTxt when ES-DE does not enter it for a reason of its own (the
    /// reason of what lies below it, unless `above` holds an earlier one).
    ScanExclusion exclusion = ScanExclusion::None;
    /// The outermost folder above the item that ES-DE does not enter, and why (Hidden, NoLoadTxt); None when ES-DE
    /// enters every folder down to it. A gamelist entry overrides this, as ES-DE applies its entries: it creates the
    /// folders it does not find, but returns early at a folder it marked noload (GamelistFileParser::findOrCreateFile).
    ScanExclusion above = ScanExclusion::None;
    /// Its extension is one of the system's (for a directory, also not "."): what decides whether it may be a game.
    bool extensionConfigured = false;
    /// A MAME BIOS or device set of an arcade system.
    bool arcadeAsset = false;
    /// The folder holding it is hidden (the attribute or a leading dot); for an item of the system folder, the system
    /// folder itself. ES-DE's gamelist parser skips an entry whose file or parent folder is hidden while
    /// ShowHiddenFiles is off.
    bool parentHidden = false;
    /// The name ES-DE gives it without metadata: the stem of a file (a directory game's name without its extension, a
    /// folder's whole name), its MAME name in an arcade system; as ES-DE shows it, never empty for a Game.
    std::string defaultName;

    /// What a gamelist entry naming it must be: a <folder> entry for a directory ES-DE enters, a <game> entry for a
    /// file or a game directory.
    [[nodiscard]] bool enteredFolder() const noexcept { return verdict == ScanVerdict::Folder; }
};

/// A system's ROM folder as scanned.
struct RomFolderListing {
    /// Every file and folder below the system folder, by key (LookupKey order), but what lies inside a directory that
    /// would be a game (ES-DE does not look into it) and below a folder that could not be listed.
    std::vector<ScannedItem> items;
    /// The folders below the system folder that could not be listed, by relative path: their content is unknown.
    std::vector<catalog::UnavailableFolder> unavailableFolders;
    /// kRomIssueFolderUnavailable, kRomIssueEntryUnreadable, kRomIssueNameUnsupported, kRomIssueNameEmpty, in the order
    /// of the walk.
    std::vector<catalog::Issue> issues;
    /// The system folder itself is hidden (the attribute): with ShowHiddenFiles off ES-DE skips the whole system.
    bool systemFolderHidden = false;
    /// The system folder holds a file or folder named exactly "noload.txt": ES-DE does not populate the system, and
    /// does not read its gamelist (SystemData::populateFolder returns early).
    bool noLoadAtRoot = false;
    /// The files and folders listed (TreeWalkSummary), the system folder not counted.
    std::uint64_t filesListed = 0;
    std::uint64_t foldersListed = 0;

    /// The items whose key is `key` (several when names differ in case only, on a case-sensitive folder).
    [[nodiscard]] std::span<const ScannedItem> sameKey(std::string_view key) const noexcept;
};

/// Scans the ROM folder `romFolder` of the system `system` with ES-DE's rules (above). Below the folder nothing is
/// fatal: folders that cannot be listed are unavailable folders, entries that cannot be listed issues. Errors: those
/// of walking the folder itself (NotFound: no such folder; InvalidArgument: not a folder; Unavailable: it cannot be
/// reached or listed), Cancelled (ScanRules::cancel).
[[nodiscard]] Expected<RomFolderListing> scanRomFolder(const std::filesystem::path& romFolder,
                                                       const catalog::SystemName& system, const ScanRules& rules);

} // namespace rv::esde
