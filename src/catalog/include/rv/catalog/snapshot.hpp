#pragma once

// The snapshot of a library an adapter reads (ILibrarySource): its systems, every entry with its state, the disc
// groups, the folders that could not be listed and what went wrong on the way. A function of the library's files alone:
// identities, history and everything else Retroverse knows on its own are the store's (ADR-0008 §5).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <rv/catalog/disc_group.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/system.hpp>

namespace rv::catalog {

/// How serious an issue is.
enum class IssueSeverity : std::uint8_t {
    Info,    ///< worth knowing, nothing is lost (an ES-DE quirk followed, a name kept apart from its namesake)
    Warning, ///< something was skipped or kept as it was (a gamelist entry outside its folder, an unreadable cue sheet)
    Error,   ///< a part of the library could not be read (a gamelist that failed, a folder that cannot be listed)
};

/// "info", "warning", "error".
[[nodiscard]] constexpr std::string_view toString(IssueSeverity severity) noexcept {
    switch (severity) {
    case IssueSeverity::Info:
        return "info";
    case IssueSeverity::Warning:
        return "warning";
    case IssueSeverity::Error:
        return "error";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<IssueSeverity> parseIssueSeverity(std::string_view text) noexcept {
    return detail::enumFromString(text, IssueSeverity::Error);
}

/// A problem met while reading a library or importing it, reported and never fatal.
struct Issue {
    IssueSeverity severity = IssueSeverity::Warning;
    /// What kind of issue it is: stable, lowercase, words joined by '-' ("gamelist-failed", "key-collision"), what
    /// tests and tools branch on. Each step defines the codes it reports next to the code that reports them.
    std::string code;
    /// The ES-DE system name it concerns, as written (it may be one that is no valid SystemName); empty when none.
    std::string system;
    /// The file or folder it concerns (absolute, UTF-8, the user's own path: never logged above debug level by the
    /// product); empty when none.
    std::string file;
    /// One sentence for people (English, for the log and the CLI).
    std::string reason;

    bool operator==(const Issue&) const = default;
};

/// A folder below a system's ROM folder that could not be listed: its entries, and the rows the store holds under it,
/// are "source unavailable", never missing (ADR-0008 §4). A system folder that cannot be listed is marked in its
/// SystemInfo instead; every other folder of a listed system was listed.
struct UnavailableFolder {
    SystemName system;
    /// Relative to the system's ROM folder, '/' between the elements (as LookupKey::relativePath()).
    std::string relativePath;
    SourceUnavailableReason reason = SourceUnavailableReason::NotListable;
    /// What the file system said.
    std::string detail;

    bool operator==(const UnavailableFolder&) const = default;
};

/// The order of a snapshot's entries: by system, then lookup key (key, then exact relative path), then gamelist path
/// (which orders the entries outside their system folder, whose keys are empty); all ordinal.
[[nodiscard]] inline bool entryOrder(const CatalogEntry& a, const CatalogEntry& b) noexcept {
    return std::tie(a.system, a.key, a.gamelistPath) < std::tie(b.system, b.key, b.gamelistPath);
}

/// A library as its adapter read it.
struct LibrarySnapshot {
    /// The profile it was read with.
    LibraryProfile profile;
    /// Every system the configuration defines (ES-DE's es_systems.xml with its custom_systems), by name (ordinal); one
    /// whose ROM folder does not exist is SourceUnavailable (FolderMissing), so that the import tells a missing folder,
    /// whose rows it keeps, from a system no longer configured.
    std::vector<SystemInfo> systems;
    /// In entryOrder(). Among the entries with a key, one per (system, relative path) at most; the entries outside
    /// their system folder have none and are one per (system, gamelist path). Those are reported in `issues` too and
    /// never indexed: without a key they get no GameId (EntryState::OutsideSystemFolder).
    std::vector<CatalogEntry> entries;
    /// By system, then head (ordinal).
    std::vector<DiscGroup> discGroups;
    /// By system, then relative path (ordinal).
    std::vector<UnavailableFolder> unavailableFolders;
    /// In the order they were met.
    std::vector<Issue> issues;

    /// The system named `name`, or nullptr.
    [[nodiscard]] const SystemInfo* findSystem(const SystemName& name) const noexcept {
        for (const SystemInfo& system : systems) {
            if (system.name == name) {
                return &system;
            }
        }
        return nullptr;
    }

    bool operator==(const LibrarySnapshot&) const = default;
};

} // namespace rv::catalog
