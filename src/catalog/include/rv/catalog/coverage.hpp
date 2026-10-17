#pragma once

// Media coverage (FR-004, ADR-0003): per system and media kind, the share of a library's entries that have a file of
// the kind, the report that tells the user what to scrape in ES-DE. Measured over four populations of a snapshot's
// entries, since "the collection" means different things: every <game> entry of the gamelists, the file there or not
// (ADR-0003's table counts these: 40,870 entries on the reference machine), those whose file exists (39,832), the games
// that have a gamelist entry (the survey's "listed games": 38,995), and every game ES-DE shows, scraped or not. The
// media are asked of an IMediaResolver: presence and extension per kind, never paths.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/media_resolver.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>

namespace rv::catalog {

/// The entries a coverage figure counts. A Folder entry belongs to none: a folder is no game.
enum class CoveragePopulation : std::uint8_t {
    /// Every <game> entry of the gamelists (CatalogEntry::fromGamelist(), any state but Folder): its file there,
    /// missing (MissingFile), not a game for ES-DE (an unconfigured extension), a disc member, under a folder that
    /// could not be listed, or outside its system folder (which has no media: ES-DE skips it). ADR-0003's denominator.
    GamelistEntries,
    /// Those whose file the scan found (Availability::Available): the entries a user can play or that ES-DE lists.
    GamelistEntriesWithFile,
    /// The games (EntryState::Game) that have a gamelist entry: what ES-DE shows of the scraped collection.
    ScrapedGames,
    /// Every game (EntryState::Game), scraped or not: what ES-DE shows, disc tracks grouped under their sheet.
    Games,
};

inline constexpr std::size_t kCoveragePopulationCount = 4;

/// Every population, in the order of their values.
inline constexpr std::array<CoveragePopulation, kCoveragePopulationCount> kCoveragePopulations{
    CoveragePopulation::GamelistEntries,
    CoveragePopulation::GamelistEntriesWithFile,
    CoveragePopulation::ScrapedGames,
    CoveragePopulation::Games,
};

/// "gamelist-entries", "gamelist-entries-with-file", "scraped-games", "games".
[[nodiscard]] constexpr std::string_view toString(CoveragePopulation population) noexcept {
    switch (population) {
    case CoveragePopulation::GamelistEntries:
        return "gamelist-entries";
    case CoveragePopulation::GamelistEntriesWithFile:
        return "gamelist-entries-with-file";
    case CoveragePopulation::ScrapedGames:
        return "scraped-games";
    case CoveragePopulation::Games:
        return "games";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<CoveragePopulation> parseCoveragePopulation(std::string_view text) noexcept {
    return detail::enumFromString(text, CoveragePopulation::Games);
}

/// Whether `entry` belongs to `population`.
[[nodiscard]] bool inPopulation(const CatalogEntry& entry, CoveragePopulation population) noexcept;

/// The counts of one population (of one system, or of all of them).
struct CoverageCounts {
    /// The members.
    std::uint64_t entries = 0;
    /// The members whose media could not be known (IMediaResolver::presence() was Unavailable: a media root or folder
    /// that cannot be read). They count in no share.
    std::uint64_t unknown = 0;
    /// The members that have a file of the kind, by kind (in the order of kMediaKinds).
    std::array<std::uint64_t, kMediaKindCount> withKind{};
    /// The members that have a file of at least one kind.
    std::uint64_t withAny = 0;

    /// The members whose media are known: the denominator of the shares.
    [[nodiscard]] std::uint64_t known() const noexcept { return entries - unknown; }
    [[nodiscard]] std::uint64_t with(MediaKind kind) const noexcept { return withKind[static_cast<std::size_t>(kind)]; }
    /// with(kind) / known(), from 0 to 1; nothing when no member's media are known.
    [[nodiscard]] std::optional<double> share(MediaKind kind) const noexcept;
    /// withAny / known(); nothing when no member's media are known.
    [[nodiscard]] std::optional<double> anyShare() const noexcept;

    /// Adds another's counts (a system's into the total).
    void add(const CoverageCounts& other) noexcept;

    bool operator==(const CoverageCounts&) const = default;
};

/// The coverage of one system (or, with an empty name, of all of them), per population.
struct SystemCoverage {
    SystemName system;
    std::array<CoverageCounts, kCoveragePopulationCount> populations{};

    [[nodiscard]] const CoverageCounts& operator[](CoveragePopulation population) const noexcept {
        return populations[static_cast<std::size_t>(population)];
    }
    [[nodiscard]] CoverageCounts& operator[](CoveragePopulation population) noexcept {
        return populations[static_cast<std::size_t>(population)];
    }

    bool operator==(const SystemCoverage&) const = default;
};

/// A library's coverage.
struct MediaCoverage {
    /// Every system with at least one member of a population, by name (ordinal).
    std::vector<SystemCoverage> systems;
    /// The systems added up (its name empty).
    SystemCoverage total;
    /// Why some members' media are not known: one per system concerned ("media-unknown", warning, the count and the
    /// resolver's first reason), by system.
    std::vector<Issue> issues;

    /// The system named `name`, or nullptr.
    [[nodiscard]] const SystemCoverage* find(const SystemName& name) const noexcept;

    bool operator==(const MediaCoverage&) const = default;
};

/// The issue code of members whose media could not be known.
inline constexpr std::string_view kCoverageIssueMediaUnknown = "media-unknown";

/// Builds a coverage entry by entry, whatever the entries come from.
class CoverageBuilder {
public:
    /// Counts `entry` in each population it belongs to, with the media it has (`media`), or as a member whose media are
    /// not known (`media` null). An entry of no population (a Folder) is left out.
    void add(const CatalogEntry& entry, const EntryMedia* media);
    /// Notes why the media of members of `system` are not known (the first reason per system is kept for the issue).
    void noteUnknown(const SystemName& system, std::string_view reason);

    /// The coverage of what was added, its systems in name order and their total.
    [[nodiscard]] MediaCoverage finish() const;

private:
    /// The members of a system whose media are not known, and why.
    struct Unknown {
        SystemName system;
        std::uint64_t entries = 0;
        std::string reason;
    };

    std::vector<SystemCoverage> m_systems; ///< by name (ordinal)
    std::vector<Unknown> m_unknown;        ///< by name (ordinal)

    SystemCoverage& systemOf(const SystemName& system);
    Unknown& unknownOf(const SystemName& system);
};

/// The coverage of `snapshot`'s entries (only those of `system` when it is given), each member's media asked of
/// `resolver` (presence(): its system, key and FileFacts::directory; false for an entry not seen). An entry outside
/// its system folder has no key and no media, and is not asked; a Folder is in no population and not asked either.
/// Members whose presence() is Unavailable count as unknown, with one issue per system. Errors: presence()'s other
/// errors, which say that something is wrong with the entries (InvalidArgument), passed on.
[[nodiscard]] Expected<MediaCoverage> measureCoverage(const LibrarySnapshot& snapshot, IMediaResolver& resolver,
                                                      const std::optional<SystemName>& system = std::nullopt);

} // namespace rv::catalog
