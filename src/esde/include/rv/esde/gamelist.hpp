#pragma once

// ES-DE's gamelists (ADR-0008 §3, §5, §7): one system's gamelist.xml read into the catalog's values. Each <game> and
// <folder> entry gives its path resolved against the system's ROM folder with its lookup key, its metadata normalized
// as rv_catalog models it, the original forms of the normalized values and every element Retroverse does not model
// (kept as written), ES-DE's flags and the per-game alternative emulator; the gamelist gives the top-level
// <alternativeEmulator> beside its <gameList>. Nothing is checked on disk: the ROM scan (I2 step 7) merges the entries
// with the files it lists (entryForListed()) and builds the snapshot. Read-only: the file is opened for reading with
// full sharing (ES-DE rewrites it after every game played from it, in place) and never written.
//
// Values are read the way ES-DE 3.4 reads them (GamelistFileParser, MetaDataList::createFromXML): an element's text is
// its first text or CDATA child, decoded, never trimmed; an element without text reads as a missing one; of an element
// repeated within an entry the first counts; only <game> and <folder> count inside <gameList>, and only the first
// <gameList> and the first <alternativeEmulator> at the top level. What ES-DE ignores is reported (Info) and dropped,
// except the elements Retroverse does not model, which are kept (catalog::RawEsdeValues), up to
// kGamelistUnknownElementsPerEntry per entry.
//
// What one file can make is bounded, whatever it holds within kMaxGamelistBytes: a path longer than any path Windows
// can name is refused before it is resolved, issues about single entries are listed up to kGamelistIssuesListedPerCode
// per code and counted beyond, ignored elements are counted per name up to kGamelistNamesCountedPerKind names, and an
// entry superseded by a later one is replaced by it at once.

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
#include <rv/catalog/metadata.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>

namespace rv::esde {

/// The largest gamelist read: ES-DE's are a few MiB (the reference library's largest, 4,462 entries, is 4 MiB; a
/// legacy gamelist of 19,380 games is 6.5 MiB).
inline constexpr std::uintmax_t kMaxGamelistBytes = 64ull * 1024ull * 1024ull;

/// The longest <path> read, in UTF-16 code units: the longest path Windows can name, in its \\?\ form (the kernel keeps
/// a path's length in bytes in 16 bits). A longer path names no file: it is skipped as malformed before it is resolved,
/// so that one huge path costs no more than its text.
inline constexpr std::size_t kGamelistMaxPathUtf16Units = 32767;

/// The most issues of one code that readGamelist() lists about single entries (kGamelistIssueEntryWithoutPath,
/// kGamelistIssuePathMalformed, kGamelistIssuePathOutsideFolder, kGamelistIssueDuplicatePath): the others of that code
/// are counted into one last issue of it, which gives their number and the line of the last one. Real gamelists have
/// none; a broken one could otherwise make an issue of almost every line.
inline constexpr std::size_t kGamelistIssuesListedPerCode = 100;

/// The most element names whose ignored elements are counted apart, per kind of element that can bear any name (the
/// top-level elements besides <gameList> and <alternativeEmulator>, the elements inside <gameList> besides <game> and
/// <folder>, those inside <alternativeEmulator> besides its <label>): the elements of further names are counted
/// together, in one issue of their kind that names the first of them.
inline constexpr std::size_t kGamelistNamesCountedPerKind = 16;

/// The most unknown elements kept per entry (catalog::RawEsdeValues::unknownElements, in document order): ES-DE's and
/// other tools' entries hold a few dozen at most; further ones are ignored elements.
inline constexpr std::size_t kGamelistUnknownElementsPerEntry = 256;

/// The codes of the issues readGamelist() reports (catalog::Issue::code). Each names the system and the gamelist file,
/// and its reason the line of the element concerned.
/// A <game> or <folder> entry without a <path> text: skipped (ES-DE skips it). Warning.
inline constexpr std::string_view kGamelistIssueEntryWithoutPath = "gamelist-entry-without-path";
/// A <path> that core::resolveRelativePath() refuses as malformed (control characters, malformed UTF-8, "..", ':' in a
/// name, a drive-relative form), that makes no lookup key, or that is longer than kGamelistMaxPathUtf16Units: the
/// entry is skipped. Warning.
inline constexpr std::string_view kGamelistIssuePathMalformed = "gamelist-path-malformed";
/// A <path> that leads out of the system's ROM folder ("./../snes/x.sfc", an absolute path elsewhere): the entry is
/// kept outside the system folder (GamelistEntry::outsideSystemFolder(), EntryState::OutsideSystemFolder), never
/// indexed. Warning.
inline constexpr std::string_view kGamelistIssuePathOutsideFolder = "gamelist-path-outside-folder";
/// Two entries of one file. An entry superseded by a later one of the same kind and the same relative path (written
/// alike or not: "./a.gb", ".\a.gb"; outside the folder, of the same path as written): the later one is kept in its
/// place. Or an entry that names the same file as an earlier one (the same lookup key) but differs from it in kind or
/// in the case of its path: both are kept, and the ROM scan uses the one that matches what is on disk
/// (entryForListed()). Warning.
inline constexpr std::string_view kGamelistIssueDuplicatePath = "gamelist-duplicate-path";
/// Values of a normalized element (<rating>, <players>, <releasedate>, a boolean, a count, <lastplayed>) that do not
/// parse: kept as written in the original forms, the value missing (a boolean false, a count 0, as ES-DE reads them).
/// One issue per element name, with the count and the first value and line. Info.
inline constexpr std::string_view kGamelistIssueValueUnreadable = "gamelist-value-unreadable";
/// Elements ES-DE ignores and Retroverse drops: a modelled element repeated within an entry (the first counts),
/// elements inside <gameList> other than <game> and <folder>, top-level elements other than the first <gameList> and
/// the first <alternativeEmulator>, an <alternativeEmulator> without a label text, elements inside it other than its
/// first <label>, the unknown elements of an entry beyond kGamelistUnknownElementsPerEntry. One issue per kind and
/// element name (up to kGamelistNamesCountedPerKind names per kind), with the count and the first line. Info.
inline constexpr std::string_view kGamelistIssueIgnoredElement = "gamelist-ignored-element";
/// The file was read leniently, as ES-DE reads it: bytes that are not UTF-8 read as Windows-1252 characters,
/// character references to no character read as U+FFFD (one issue per kind, from the XML loader). Warning.
inline constexpr std::string_view kGamelistIssueReadLeniently = "gamelist-read-leniently";

/// What a gamelist entry is: its element's name.
enum class GamelistEntryKind : std::uint8_t {
    Game,   ///< <game>: a file (or a folder named with an extension) with its metadata
    Folder, ///< <folder>: the metadata of a sub-folder of the ROM folder
};

/// "game", "folder": the element's name.
[[nodiscard]] constexpr std::string_view toString(GamelistEntryKind kind) noexcept {
    return kind == GamelistEntryKind::Folder ? "folder" : "game";
}

/// One <game> or <folder> entry, as the ROM scan merges it with the files it lists: the gamelist side of a
/// catalog::CatalogEntry.
struct GamelistEntry {
    GamelistEntryKind kind = GamelistEntryKind::Game;
    /// The <path> exactly as written, decoded ("./Collection A/Pocket Puzzle (World).gb"): CatalogEntry::gamelistPath.
    std::string path;
    /// The path relative to the system's ROM folder (core::resolveRelativePath(): what lies below the folder, as
    /// written, '/' between the elements, no "./") and its lookup key, by which the scan matches the entry to the file
    /// it names, also one whose name differs in case only (ADR-0008 §3). Empty for an entry outside the system folder.
    catalog::LookupKey key;
    /// <name>, <sortname>, <desc>, <developer>, <publisher>, <genre> exactly as written; <releasedate>, <rating> and
    /// <players> parsed, nothing when missing, empty or unreadable.
    catalog::GameMetadata metadata;
    /// The original forms of the normalized elements present (an empty one as ""), and every element Retroverse does
    /// not model (<controller>, <collectionsortname>, <folderlink>, other tools' <region> or <md5>, ...) with its text,
    /// in document order, up to kGamelistUnknownElementsPerEntry of them.
    catalog::RawEsdeValues raw;
    /// <favorite>, <hidden>, <kidgame>, <completed>, <broken> (true only for "true", as ES-DE reads them),
    /// <playcount>, <playtime>, <lastplayed>.
    catalog::EsdeFlags flags;
    /// <altemulator> as written: a command label of the system, used when AlternativeEmulatorPerGame is on (ADR-0008
    /// §8, not checked here); empty when none.
    std::string altEmulator;
    /// The line of the entry's element in the file (1-based), for the issues the scan reports about the entry.
    std::size_t line = 0;

    /// The path leads out of the system's ROM folder: the snapshot keeps the entry as OutsideSystemFolder, without a
    /// key, and never indexes it.
    [[nodiscard]] bool outsideSystemFolder() const noexcept { return key.empty(); }

    bool operator==(const GamelistEntry&) const = default;
};

/// A system's gamelist as readGamelist() read it.
struct Gamelist {
    /// The <label> of the top-level <alternativeEmulator> (before or after <gameList>; ES-DE writes it before), as
    /// written: the command the system launches with when neither the game nor Retroverse chooses one
    /// (SystemInfo::alternativeEmulator). Empty when there is none or its label is empty. Not checked against the
    /// system's commands here (launch translation, I2 step 10, does).
    std::string alternativeEmulator;
    /// The <game> and <folder> entries in document order, one per kind and relative path (outside the system folder,
    /// one per path as written). Of several entries of one kind and path the last one is kept, as in ES-DE, which
    /// applies a gamelist's entries to the file they name in turn (the last one's metadata shows) and saves an updated
    /// entry by removing the first node of its path and appending the new one. Entries that name one file but differ in
    /// kind or in the case of their path (the same lookup key) are all kept: only the disk tells which one ES-DE
    /// applies, and the ROM scan chooses with entryForListed(). Both cases are kGamelistIssueDuplicatePath issues.
    /// Entries without a usable path are left out (kGamelistIssueEntryWithoutPath, kGamelistIssuePathMalformed).
    std::vector<GamelistEntry> entries;
    /// What was skipped, superseded, ignored or read leniently: the file's lenient reading first, then the others in
    /// document order (an issue that counts several elements at its first one), then the counts of the issues about
    /// single entries not listed one by one (kGamelistIssuesListedPerCode).
    std::vector<catalog::Issue> issues;

    /// The number of entries of a kind.
    [[nodiscard]] std::size_t count(GamelistEntryKind kind) const noexcept {
        std::size_t n = 0;
        for (const GamelistEntry& entry : entries) {
            n += entry.kind == kind ? 1 : 0;
        }
        return n;
    }
    /// The first entry whose path is exactly `path` as written, or nullptr.
    [[nodiscard]] const GamelistEntry* find(std::string_view path) const noexcept {
        for (const GamelistEntry& entry : entries) {
            if (entry.path == path) {
                return &entry;
            }
        }
        return nullptr;
    }
    /// The entry of kind `kind` whose path is exactly `path` as written, or nullptr.
    [[nodiscard]] const GamelistEntry* find(std::string_view path, GamelistEntryKind kind) const noexcept {
        for (const GamelistEntry& entry : entries) {
            if (entry.kind == kind && entry.path == path) {
                return &entry;
            }
        }
        return nullptr;
    }

    bool operator==(const Gamelist&) const = default;
};

/// Where ES-DE keeps the gamelist of `system` (not empty): <esdeHome>/gamelists/<system name>/gamelist.xml. The only
/// place Retroverse reads gamelists from: a gamelist.xml inside a ROM folder is a legacy one that ES-DE ignores while
/// LegacyGamelistFileLocation is off (ADR-0008 §3), and Retroverse never reads it.
[[nodiscard]] std::filesystem::path gamelistPath(const std::filesystem::path& esdeHome,
                                                 const catalog::SystemName& system);

/// Reads the gamelist `file` of the system `system`, whose ROM folder is `romFolder` (absolute: SystemInfo::romFolder),
/// up to `maxBytes`: decoded as ES-DE decodes it (rv_esde's XML loader; what was read leniently becomes issues and is
/// logged at debug level only, the file's path being the user's), every <path> resolved against `romFolder`
/// (core::resolveRelativePath(); nothing is read under it). A <gameList> without entries is a valid, empty gamelist.
/// Errors (the file as their context): NotFound when the file does not exist (the system has no gamelist:
/// GamelistState::Absent); every other error is a failure the caller retries after a delay before it gives up on the
/// system's metadata (ADR-0008 §7: GamelistState::Failed): ParseError for an empty file, a file without <gameList>,
/// one cut short or padded with NUL characters while ES-DE writes it, or nested deeper than the loader allows;
/// IoError (it shrank while it was read), Conflict (locked), Unavailable (its drive cannot be reached), OutOfRange
/// (larger than `maxBytes`, or too large for the memory available), InvalidArgument (a folder, or a `romFolder` that is
/// not absolute).
[[nodiscard]] Expected<Gamelist> readGamelist(const std::filesystem::path& file, const catalog::SystemName& system,
                                              const std::filesystem::path& romFolder,
                                              std::uintmax_t maxBytes = kMaxGamelistBytes);

/// The entry of a gamelist that applies to a file or folder the ROM scan lists, as ES-DE 3.4 applies them, or nullptr
/// when `sameKey` is empty. `sameKey` holds the entries of one Gamelist whose lookup key is that of `listed`, in the
/// order of Gamelist::entries (several when they differ in kind or in the case of their path); `listed` is the relative
/// path as listed on disk with its key; `kind` is what ES-DE makes of it: Folder for a directory it enters, Game for a
/// file or a directory whose name carries a configured extension. In this order:
///  1. the entry of that kind spelled exactly like `listed` (relative paths compared byte for byte): the one ES-DE
///     applies, as it finds a gamelist entry's file by its exact name;
///  2. else the last entry of that kind: a path that differs from the file in case only matches it (ADR-0008 §3; ES-DE
///     would show it as a second entry beside the file);
///  3. else the last entry, of the other kind: ES-DE skips an entry whose kind does not match the file ("Game/folder
///     mismatch"), and so does the scan, with an issue.
[[nodiscard]] const GamelistEntry* entryForListed(std::span<const GamelistEntry* const> sameKey,
                                                  const catalog::LookupKey& listed, GamelistEntryKind kind) noexcept;

} // namespace rv::esde
