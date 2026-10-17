#pragma once

// Disc groups (ADR-0008 §3): the files a .cue sheet or an .m3u playlist references become its disc members, when the
// sheet is itself a game of the same system. ES-DE lists every track of a cue sheet as a game (psx: 15,339 entries for
// 2,518 discs on the reference machine); the catalog keeps the tracks identifiable but hidden behind their head. A
// deliberate difference from ES-DE, which never reads a sheet. Here:
//  - reading a sheet: decodeSheet() (UTF-8, a byte order mark skipped, bytes that are not UTF-8 read as Windows-1252),
//    cueSheetFiles() (the names of its FILE lines), m3uFiles() (its lines but comments), resolveSheetReference() (a
//    name against the sheet's folder, as a path below the system folder whose key matches a listed file with the key's
//    case folding), readDiscSheet() (all of it on one file);
//  - grouping: groupDiscs(), from each head's references to groups without cycles. A chain (an .m3u naming cue sheets
//    that name tracks) gives a group per head: the cue sheets are members of the playlist and heads of their tracks.
// Which files may be members, and the cache of groups read before (catalog::LibraryLoadOptions::discGroups), are the
// library source's (EsdeLibrarySource).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/catalog/disc_group.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>

namespace rv::esde {

/// The largest sheet read: a cue sheet of 99 tracks is about 10 KiB.
inline constexpr std::uintmax_t kMaxDiscSheetBytes = 1024ull * 1024ull;

/// The codes of the issues the disc grouping reports (catalog::Issue::code), each naming the system and the sheet.
/// A sheet that is a game but cannot be read (or is larger than kMaxDiscSheetBytes): its group read before is kept
/// (catalog::LibraryLoadOptions::discGroups), whatever its size and time; without one its files stay what the listing
/// makes them. Warning.
inline constexpr std::string_view kDiscIssueSheetUnreadable = "disc-sheet-unreadable";
/// A reference of a sheet that names no file or folder of the system folder: a track or disc that is not there. Warning.
inline constexpr std::string_view kDiscIssueReferenceMissing = "disc-reference-missing";
/// A reference that leads out of the system folder, or that is no path (empty after decoding, a control character, a
/// drive-relative form): ignored. Warning.
inline constexpr std::string_view kDiscIssueReferenceInvalid = "disc-reference-invalid";
/// A head that references, through its members, itself: the reference that closes the cycle is dropped, so that one
/// head of the cycle stays a game. Warning.
inline constexpr std::string_view kDiscIssueCycle = "disc-group-cycle";

/// The kind of sheet a file is by its name's extension, compared ignoring ASCII case (".cue", ".CUE", ".m3u"); nothing
/// for any other name.
[[nodiscard]] std::optional<catalog::DiscGroupKind> discSheetKind(std::string_view name) noexcept;

/// A sheet's text as UTF-8.
struct SheetText {
    std::string text;
    /// Bytes that are not UTF-8 were read as Windows-1252 characters (core::utf8WithWindows1252Fallback): a sheet
    /// written by an older tool in the ANSI code page.
    bool windows1252 = false;
};

/// Decodes a sheet's bytes: a UTF-8 byte order mark skipped, every well-formed UTF-8 sequence kept, every other byte
/// read as its Windows-1252 character.
[[nodiscard]] SheetText decodeSheet(std::string_view bytes);

/// The file names of a cue sheet's FILE lines, in order, as written: `FILE "<name>" <type>`, or `FILE <name> <type>`
/// without quotes, the type being the last word when there are several (BINARY, WAVE, MP3, ...). The keyword compares
/// ignoring ASCII case; blanks before it, CR and LF line ends and a missing closing quote (the name runs to the line's
/// end) are accepted.
[[nodiscard]] std::vector<std::string> cueSheetFiles(std::string_view text);

/// The entries of an .m3u playlist, in order: every line trimmed of blanks, but empty lines and comments (lines that
/// start with '#', the extended M3U directives among them).
[[nodiscard]] std::vector<std::string> m3uFiles(std::string_view text);

/// A sheet's reference as a path below the system folder ('/' between the elements, as LookupKey::relativePath()):
/// `reference` (a name or a relative path, '/' or '\' between its elements, "." and ".." resolved; or an absolute path
/// inside `systemFolder`) taken from the folder of the sheet `sheetRelativePath` (a path below `systemFolder`). Errors
/// (the reference as context): OutOfRange when it leads out of the system folder; InvalidArgument when it is no path
/// (empty, a control character, ':' but after a drive letter, a drive-relative or rooted form).
[[nodiscard]] Expected<std::string> resolveSheetReference(std::string_view reference,
                                                          std::string_view sheetRelativePath,
                                                          const std::filesystem::path& systemFolder);

/// A sheet as read.
struct DiscSheet {
    catalog::DiscGroupKind kind = catalog::DiscGroupKind::Cue;
    /// Its references in order, as written (decoded): what cueSheetFiles() or m3uFiles() gives.
    std::vector<std::string> references;
    bool windows1252 = false;
};

/// Reads the sheet `sheet` of the system folder `systemFolder` (opened through its \\?\ form below the folder, for
/// reading only and with full sharing: core::readFileShared), up to kMaxDiscSheetBytes. Errors: those of reading it
/// (NotFound, Unavailable, Conflict, OutOfRange, IoError, InvalidArgument for a folder), the sheet as context.
[[nodiscard]] Expected<DiscSheet> readDiscSheet(const std::filesystem::path& systemFolder,
                                                const catalog::LookupKey& sheet, catalog::DiscGroupKind kind);

/// A head and the entries it references that may be its members: games of the same system, the head excluded, each
/// once, in the order the head names them.
struct DiscHeadLinks {
    catalog::LookupKey head;
    catalog::DiscGroupKind kind = catalog::DiscGroupKind::Cue;
    std::vector<catalog::LookupKey> members;
};

/// The groups of one system.
struct DiscGrouping {
    /// One per head that keeps at least one member, by head (LookupKey order), each with its members in the head's
    /// order.
    std::vector<catalog::DiscGroup> groups;
    /// Every member with the head it is shown under (CatalogEntry::discHead), by member (LookupKey order): of several
    /// heads that reference it (a track two cue sheets name), the first in LookupKey order.
    std::vector<std::pair<catalog::LookupKey, catalog::LookupKey>> memberHeads;
    /// The indices of `groups` with every group whose head is a member of another group before that group: the order
    /// in which a head without metadata of its own takes a member's (members first).
    std::vector<std::size_t> membersFirst;
    /// kDiscIssueCycle, one per reference dropped.
    std::vector<catalog::Issue> issues;

    /// The head `member` is shown under, or nullptr when it is no member.
    [[nodiscard]] const catalog::LookupKey* headOf(const catalog::LookupKey& member) const noexcept;
};

/// Groups the heads of the system `system`: each head's members as `links` give them (any order of heads). A reference
/// that closes a cycle (a head reaching itself through its members, a playlist naming another that names it back) is
/// dropped, the heads being walked in LookupKey order and depth first, so that the cycle keeps its first head as a game
/// and the others as its members; `systemFolder` names the files in the issues.
[[nodiscard]] DiscGrouping groupDiscs(const catalog::SystemName& system, std::vector<DiscHeadLinks> links,
                                      const std::filesystem::path& systemFolder);

} // namespace rv::esde
