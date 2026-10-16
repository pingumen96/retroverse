#pragma once

// The entries of a library snapshot and their states (ADR-0008 §3). An entry is a file or folder of a system that ES-DE
// would list, or that a gamelist entry names: what ES-DE shows is a game; the rest keeps a state that says why it is
// not one, so that a setting turned off or a gamelist entry left behind never reads as a deleted game. Files whose
// extension the system does not configure and that no gamelist entry names are no entries at all, and neither is a
// <folder> entry that names no folder (its folder gone, or a file of that name, where the folder above it was
// listed): ES-DE skips it, and the adapter reports it as an issue; a folder is no game, so nothing is kept for it as
// unavailable.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <ratio>
#include <string>
#include <string_view>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

/// What an entry is (ADR-0008 §3). Only MissingFile is unavailable, and only it takes part in reconciliation.
enum class EntryState : std::uint8_t {
    Game,                ///< what ES-DE shows: a file (or a folder named with an extension) the system configures
    MissingFile,         ///< a gamelist entry whose file is gone: kept with its metadata, hidden by default (FR-033)
    NotAGame,            ///< the file exists but ES-DE would not show it (NotAGameReason)
    DiscMember,          ///< a file that a .cue or .m3u of the same system references, that .cue or .m3u being a game
                         ///< itself: kept and identifiable, hidden (CatalogEntry::discHead)
    Folder,              ///< a <folder> entry of a gamelist: the metadata of a sub-folder (FileFacts::directory)
    OutsideSystemFolder, ///< a gamelist entry whose path leads out of the system's ROM folder (ES-DE skips it): in the
                         ///< snapshot only, reported as an issue too; it has no key, so it never gets a GameId and is
                         ///< never indexed or queried
};

/// "game", "missing-file", "not-a-game", "disc-member", "folder", "outside-system-folder".
[[nodiscard]] constexpr std::string_view toString(EntryState state) noexcept {
    switch (state) {
    case EntryState::Game:
        return "game";
    case EntryState::MissingFile:
        return "missing-file";
    case EntryState::NotAGame:
        return "not-a-game";
    case EntryState::DiscMember:
        return "disc-member";
    case EntryState::Folder:
        return "folder";
    case EntryState::OutsideSystemFolder:
        return "outside-system-folder";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<EntryState> parseEntryState(std::string_view text) noexcept {
    return detail::enumFromString(text, EntryState::OutsideSystemFolder);
}

/// Why an existing file is NotAGame (ADR-0008 §3).
enum class NotAGameReason : std::uint8_t {
    None,                   ///< the entry is not NotAGame
    ExtensionNotConfigured, ///< a gamelist entry names a file whose extension the system does not configure (compared
                            ///< case-sensitively, as ES-DE does: ps2's ".cue" on the reference machine)
    NoLoadTxt,              ///< a noload.txt in its folder or above it excludes it
    BiosOrDeviceSet,        ///< a MAME BIOS or device set (the arcade and neogeo platforms)
    ExcludedBySetting,      ///< an ES-DE setting excludes it (CatalogEntry::excludingSetting names it): ShowHiddenFiles
                            ///< off for a hidden file, ShowHiddenGames off for a file its gamelist entry marks <hidden>
};

/// "none", "extension-not-configured", "noload-txt", "bios-or-device-set", "excluded-by-setting".
[[nodiscard]] constexpr std::string_view toString(NotAGameReason reason) noexcept {
    switch (reason) {
    case NotAGameReason::None:
        return "none";
    case NotAGameReason::ExtensionNotConfigured:
        return "extension-not-configured";
    case NotAGameReason::NoLoadTxt:
        return "noload-txt";
    case NotAGameReason::BiosOrDeviceSet:
        return "bios-or-device-set";
    case NotAGameReason::ExcludedBySetting:
        return "excluded-by-setting";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<NotAGameReason> parseNotAGameReason(std::string_view text) noexcept {
    return detail::enumFromString(text, NotAGameReason::ExcludedBySetting);
}

/// Whether an entry's file is there (ADR-0008 §4).
enum class Availability : std::uint8_t {
    Available,         ///< its folder was listed and the file (or folder) is there
    SourceUnavailable, ///< its folder could not be listed or reached (SourceUnavailableReason): nothing is known about
                       ///< the file, which is never taken as gone and stays out of reconciliation
    Missing,           ///< its folder was listed and the file is not there: the MissingFile entries, and only them
};

/// "available", "source-unavailable", "missing".
[[nodiscard]] constexpr std::string_view toString(Availability availability) noexcept {
    switch (availability) {
    case Availability::Available:
        return "available";
    case Availability::SourceUnavailable:
        return "source-unavailable";
    case Availability::Missing:
        return "missing";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<Availability> parseAvailability(std::string_view text) noexcept {
    return detail::enumFromString(text, Availability::Missing);
}

/// Why a folder (a system's ROM folder, or one below it) could not be listed.
enum class SourceUnavailableReason : std::uint8_t {
    None,          ///< the source is available
    FolderMissing, ///< the folder does not exist (its system's folder, or the ROM root above it, is gone or renamed)
    Unreachable,   ///< its drive, share or device cannot be reached (an unplugged drive, an unmapped letter)
    NotListable,   ///< it exists but could not be listed: access denied, an I/O error, a link that loops
};

/// "none", "folder-missing", "unreachable", "not-listable".
[[nodiscard]] constexpr std::string_view toString(SourceUnavailableReason reason) noexcept {
    switch (reason) {
    case SourceUnavailableReason::None:
        return "none";
    case SourceUnavailableReason::FolderMissing:
        return "folder-missing";
    case SourceUnavailableReason::Unreachable:
        return "unreachable";
    case SourceUnavailableReason::NotListable:
        return "not-listable";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<SourceUnavailableReason>
parseSourceUnavailableReason(std::string_view text) noexcept {
    return detail::enumFromString(text, SourceUnavailableReason::NotListable);
}

/// File times as the catalog keeps them: FILETIME ticks, 100 ns since 1601-01-01 UTC, the file system's own unit and
/// epoch (std::filesystem::file_time_type counts exactly these on Windows).
using FileTimeTicks = std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;

[[nodiscard]] inline std::int64_t fileTimeTicks(std::filesystem::file_time_type time) noexcept {
    return std::chrono::duration_cast<FileTimeTicks>(time.time_since_epoch()).count();
}
[[nodiscard]] inline std::filesystem::file_time_type fileTimeFromTicks(std::int64_t ticks) noexcept {
    return std::filesystem::file_time_type(
        std::chrono::duration_cast<std::filesystem::file_time_type::duration>(FileTimeTicks(ticks)));
}

/// A file or folder as the listing of the ROM folder showed it (ADR-0008 §4: size and time come free with the listing;
/// no ROM byte is read for them).
struct FileFacts {
    /// A folder: a <folder> entry's, or a game that is a folder whose name carries a configured extension.
    bool directory = false;
    /// Bytes (a followed link's target's); 0 for a folder.
    std::uint64_t size = 0;
    /// The last write time as the file system records it, in FILETIME ticks (fileTimeTicks()), as the store keeps it.
    std::int64_t lastWriteTime = 0;
    /// The hidden attribute or a name that starts with a dot: what ES-DE's ShowHiddenFiles tests.
    bool hidden = false;

    bool operator==(const FileFacts&) const = default;
};

/// What ES-DE records in a gamelist entry about the user's use of the game (read, never written: ADR-0008 §7).
struct EsdeFlags {
    bool favorite = false; ///< <favorite>
    /// <hidden>. With ES-DE's ShowHiddenGames off, ES-DE drops such an entry when it loads the gamelist (an exclusion,
    /// not a filter): an existing file whose entry is marked so is NotAGame (ExcludedBySetting, "ShowHiddenGames").
    /// With the setting on, it is a game like any other and the flag is only kept.
    bool hidden = false;
    bool kidGame = false;                    ///< <kidgame>: shown in ES-DE's kid mode
    bool completed = false;                  ///< <completed>
    bool broken = false;                     ///< <broken>
    std::uint64_t playCount = 0;             ///< <playcount>, 0 when missing or unreadable
    std::uint64_t playTimeSeconds = 0;       ///< <playtime>, in seconds
    std::optional<LocalDateTime> lastPlayed; ///< <lastplayed>, ES-DE's local time when the game ended

    bool operator==(const EsdeFlags&) const = default;
};

/// One entry of a library snapshot (ILibrarySource). GameIds are not part of it: the import assigns them (identity.hpp)
/// and the store keeps them.
struct CatalogEntry {
    SystemName system;
    /// The path relative to the system's ROM folder and its key; empty for OutsideSystemFolder (gamelistPath holds the
    /// path as written).
    LookupKey key;
    EntryState state = EntryState::Game;
    /// Set for NotAGame only.
    NotAGameReason notAGameReason = NotAGameReason::None;
    /// For NotAGameReason::ExcludedBySetting: the ES-DE setting's name as es_settings.xml writes it ("ShowHiddenFiles",
    /// "ShowHiddenGames").
    std::string excludingSetting;
    /// Missing exactly for MissingFile; SourceUnavailable for an entry under a folder that could not be listed (its
    /// state then says what the gamelist alone tells: Game for a <game> entry with a configured extension).
    Availability availability = Availability::Available;
    /// Set for SourceUnavailable only.
    SourceUnavailableReason unavailableReason = SourceUnavailableReason::None;
    /// The file or folder as listed; nothing when it was not seen (MissingFile, OutsideSystemFolder,
    /// SourceUnavailable).
    std::optional<FileFacts> file;
    /// The <path> of the gamelist entry exactly as written ("./Collection A/Pocket Puzzle (World).gb"); empty for an
    /// entry that no gamelist entry names.
    std::string gamelistPath;
    /// The title shown: displayText() of the gamelist's <name>, else what the adapter gives a file without metadata
    /// (titleFromFileStem(), a MAME name for an arcade set). Never empty for a Game.
    std::string title;
    GameMetadata metadata;
    RawEsdeValues raw;
    /// For a DiscMember: the head of its group (a .cue or .m3u of the same system, DiscGroup::head). Empty otherwise.
    LookupKey discHead;
    /// The gamelist entry's <altemulator>: an ES-DE command label of the system, used when AlternativeEmulatorPerGame
    /// is on (ADR-0008 §8). Empty when none.
    std::string altEmulator;
    EsdeFlags flags;

    /// A gamelist entry names it.
    [[nodiscard]] bool fromGamelist() const noexcept { return !gamelistPath.empty(); }

    bool operator==(const CatalogEntry&) const = default;
};

/// The entry's sort key (sortKeyOf(), version kSortKeyVersion): of its <sortname> when that is not blank, else of its
/// title.
[[nodiscard]] inline Expected<std::string> sortKeyOf(const CatalogEntry& entry) {
    const std::string sortName = displayText(entry.metadata.sortName);
    return sortKeyOf(sortName.empty() ? std::string_view(entry.title) : std::string_view(sortName));
}

} // namespace rv::catalog
