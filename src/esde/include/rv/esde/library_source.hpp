#pragma once

// EsdeLibrarySource (ADR-0008 §2, §3, §4): catalog::ILibrarySource over an ES-DE installation. A load reads the
// configuration (settings, roots, systems, the MAME lists for the arcade systems), then every system: its ROM folder
// with ES-DE's discovery rules (scanRomFolder()), its gamelist (readGamelist(), read again after a delay when it
// fails), both merged into entries with their states, then its disc groups (disc_groups.hpp). Read-only towards ES-DE:
// files are opened for reading with full sharing, nothing is written under any of its folders.
//
// What a game is follows what ES-DE 3.4.1 would show (SystemData::populateFolder, GamelistFileParser::parseGamelist),
// so that both frontends agree:
//  - a listed file or game directory without a gamelist entry: a game as scanRomFolder() says; excluded by noload.txt,
//    a BIOS or device set or ShowHiddenFiles, it is NotAGame with that reason; a file whose extension is not configured
//    is no entry; with ParseGamelistOnly on, ES-DE lists only what its gamelist names, so such a file is NotAGame
//    (ExcludedBySetting, "ParseGamelistOnly");
//  - a listed file with a <game> entry, in ES-DE's order of checks: ShowHiddenFiles off and the file or the folder
//    holding it hidden (only those two: ES-DE creates the folders above that it skipped) → excluded by that setting; a
//    folder above it that holds a noload.txt (the outermost folder ES-DE does not enter) → noload; its extension not
//    configured → ExtensionNotConfigured; a BIOS or device set → BiosOrDeviceSet; <hidden> with ShowHiddenGames off →
//    excluded by that setting; otherwise a game with the entry's metadata. An entry matches its file by lookup key:
//    spelled exactly like it, else differing in case only (gamelist.hpp, entryForListed());
//  - a <folder> entry of a folder ES-DE enters: a Folder (also one that holds no game, which ES-DE would not show), or
//    NotAGame when the rules above exclude it; of a game directory or a file, skipped with an issue, as ES-DE skips a
//    "Game/folder mismatch";
//  - a <game> entry whose file is not listed: MissingFile (kept with its metadata, FR-033), but below a folder that
//    could not be listed or in a system folder that is not available (SourceUnavailable, with the state the gamelist
//    alone tells: the name's extension, a BIOS name, a leading dot, <hidden>) and inside a game directory (skipped
//    with an issue: ES-DE never looks there). A <folder> entry whose folder is not listed is skipped with an issue;
//  - the system folder hidden (with ShowHiddenFiles off) or holding a noload.txt (ParseGamelistOnly off): ES-DE does
//    not load the system, so every would-be game is NotAGame for that reason;
//  - a system name defined more than once (a custom system and the bundled one, SystemsConfiguration::fallbacks): the
//    first definition whose folder gives at least one game, as ES-DE loads it; the first one when none does;
//  - disc members: the games a .cue or .m3u game of the same system references (DiscMember, CatalogEntry::discHead);
//    each sheet read once per import (LibraryLoadOptions::discGroups: a group read before is used while its head keeps
//    its size and time, and kept, with an issue, while the sheet cannot be read); a head without a gamelist entry of
//    its own shows the metadata of the first of its members that has some (members first along a chain).
// Deliberate differences, besides the missing files and the disc members: a gamelist path that differs from its file
// in case only matches it (ES-DE would show a second entry); flatten.txt is ignored; gamelists inside ROM folders
// (LegacyGamelistFileLocation) are never read.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>

namespace rv::esde {

/// The codes of the issues EsdeLibrarySource reports besides those of the parts it uses (settings, roots, systems,
/// gamelists, MAME lists, the ROM scan, the disc groups), each naming the system and the file or folder.
/// A system folder that exists but cannot be reached or listed: its gamelist's entries are kept as source unavailable.
/// Error.
inline constexpr std::string_view kSourceIssueSystemUnavailable = "system-folder-unavailable";
/// A system folder that is not there although the system's gamelist names entries: they are kept as source
/// unavailable. Warning.
inline constexpr std::string_view kSourceIssueSystemMissing = "system-folder-missing";
/// A gamelist that still fails after the delay (EsdeLibrarySourceOptions::gamelistRetryDelay): the system's entries
/// have no metadata, and the import keeps the system's previous rows (GamelistState::Failed). Error.
inline constexpr std::string_view kSourceIssueGamelistFailed = "gamelist-failed";
/// A gamelist entry whose kind does not match what it names: a <game> entry naming a folder ES-DE enters, a <folder>
/// entry naming a file or a game directory. Skipped, as ES-DE skips a "Game/folder mismatch". Warning.
inline constexpr std::string_view kSourceIssueKindMismatch = "gamelist-kind-mismatch";
/// A <folder> entry that names no folder (gone, never there): skipped, as ES-DE skips it; nothing is kept for it.
/// Warning.
inline constexpr std::string_view kSourceIssueFolderNotFound = "gamelist-folder-not-found";
/// A gamelist entry naming something inside a game directory, which ES-DE never looks into: skipped. Warning.
inline constexpr std::string_view kSourceIssueInsideGame = "gamelist-entry-inside-game";
/// LegacyGamelistFileLocation is on: ES-DE then reads a gamelist.xml inside a ROM folder first; Retroverse reads only
/// <ES-DE home>/gamelists (ADR-0008 §3). Warning.
inline constexpr std::string_view kSourceIssueLegacyGamelists = "gamelist-location-unsupported";
/// ES-DE does not load the system: its folder is hidden (ShowHiddenFiles off) or holds a noload.txt. Its would-be
/// games are NotAGame for that reason. Info.
inline constexpr std::string_view kSourceIssueSystemNotLoaded = "system-not-loaded";
/// A later definition of a system name was used, as ES-DE uses it when the earlier ones find no game. Info.
inline constexpr std::string_view kSourceIssueFallbackUsed = "system-fallback-used";

/// The most issues of one code listed per system; the others of the code are counted into one more issue of it.
inline constexpr std::size_t kSourceIssuesListedPerCode = 100;

/// How an EsdeLibrarySource reads.
struct EsdeLibrarySourceOptions {
    /// How long to wait before a gamelist that failed is read again (ES-DE may be writing it, ADR-0008 §7); zero reads
    /// it again at once. The wait ends early when the load is cancelled.
    std::chrono::milliseconds gamelistRetryDelay{1000};
    /// Only these systems (names as es_systems.xml writes them); every system when empty. A snapshot of some systems is
    /// for looking at them (retroverse-cli import --dry-run --system), never for an import, which would take the others
    /// as no longer configured.
    std::vector<std::string> systems;
};

/// What the last load did, for people (retroverse-cli import --dry-run, the import's log).
struct EsdeLoadStatistics {
    std::size_t systemsConfigured = 0; ///< systems in the snapshot
    std::size_t systemsScanned = 0;    ///< ROM folders walked (fallback definitions included)
    std::uint64_t filesListed = 0;     ///< files and folders listed below the system folders
    std::uint64_t foldersListed = 0;
    std::size_t gamelistsRead = 0;    ///< read and parsed
    std::size_t gamelistRetries = 0;  ///< gamelists read a second time after a failure
    std::size_t sheetsRead = 0;       ///< cue sheets and playlists read
    std::size_t sheetsFromCache = 0;  ///< heads whose cached group was used without reading them
    std::size_t sheetsUnreadable = 0; ///< heads that could not be read
    std::chrono::nanoseconds configurationTime{};
    std::chrono::nanoseconds scanTime{};     ///< walking the ROM folders
    std::chrono::nanoseconds gamelistTime{}; ///< reading the gamelists, the retry delays included
    std::chrono::nanoseconds sheetTime{};    ///< reading the sheets
    std::chrono::nanoseconds totalTime{};
};

/// The ES-DE adapter of catalog::ILibrarySource.
class EsdeLibrarySource final : public catalog::ILibrarySource {
public:
    explicit EsdeLibrarySource(EsdeLibrarySourceOptions options = {});

    /// Reads the library `profile` describes (the ES-DE home, the install folder, the root overrides) into a snapshot,
    /// as described above. Errors: the ES-DE home is no folder (NotFound) or cannot be reached (Unavailable); the
    /// settings or the systems cannot be read (their errors: ParseError, NotFound for a missing bundled
    /// es_systems.xml, ...); a system of EsdeLibrarySourceOptions::systems that the configuration does not define
    /// (InvalidArgument); Cancelled (LibraryLoadOptions::cancel), checked between systems, within the walk of a folder,
    /// between sheets and during the gamelist delay.
    [[nodiscard]] Expected<catalog::LibrarySnapshot> load(const catalog::LibraryProfile& profile,
                                                          const catalog::LibraryLoadOptions& options) override;

    /// What the last load() did (also a load that failed, up to where it stopped).
    [[nodiscard]] const EsdeLoadStatistics& lastStatistics() const noexcept { return m_statistics; }

private:
    EsdeLibrarySourceOptions m_options;
    EsdeLoadStatistics m_statistics;
};

} // namespace rv::esde
