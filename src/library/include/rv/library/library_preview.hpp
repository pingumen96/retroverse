#pragma once

// GPU-free (rv_library, ADR-0018 §4, §13): what a library holds, read without writing a thing: `retroverse-cli import
// --dry-run`'s reading, and the wizard's page 3. esde::EsdeLibrarySource reads ES-DE's settings and systems, every
// system's ROM folder and gamelist and every cue sheet that is a game (there is no database to take the groups read
// before from).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <rv/catalog/coverage.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/expected.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/settings.hpp>

namespace rv::library {

/// One system's entries counted, or all of them.
struct PreviewCounts {
    std::size_t entries = 0;
    std::size_t games = 0;
    std::size_t missing = 0;
    std::size_t notAGame = 0;
    std::size_t members = 0;
    std::size_t folders = 0;
    std::size_t outside = 0;
    std::size_t unavailable = 0; ///< entries whose folder could not be listed (any state)
    std::size_t esde = 0;        ///< games and members on disk: what ES-DE lists
    std::size_t groups = 0;
    std::size_t unavailableFolders = 0;
    std::size_t issues = 0;
    /// "extension-not-configured", "noload-txt", "bios-or-device-set", "excluded-by-setting:<setting>".
    std::map<std::string, std::size_t> reasons;

    void add(const PreviewCounts& other);
};

/// The counts of every system of `snapshot`, by name.
[[nodiscard]] std::map<std::string, PreviewCounts> previewCounts(const catalog::LibrarySnapshot& snapshot);

/// A library read.
struct LibraryPreview {
    catalog::LibrarySnapshot snapshot;
    esde::EsdeLoadStatistics statistics;
    std::map<std::string, PreviewCounts> counts;
    /// The systems added up.
    PreviewCounts total;
};

struct PreviewOptions {
    /// Only this system (a partial snapshot, for looking at that system, never for an import).
    std::optional<std::string> system;
    /// Read between pieces of work when not null: once it holds true, the read ends with Cancelled.
    const std::atomic<bool>* cancel = nullptr;
};

/// Reads the library `profile` describes. Read-only towards ES-DE. Errors: EsdeLibrarySource::load()'s (InvalidArgument
/// for a system the configuration does not define; NotFound, Unavailable or ParseError when the ES-DE home, its settings
/// or its systems cannot be read; Cancelled).
[[nodiscard]] Expected<LibraryPreview> previewLibrary(const catalog::LibraryProfile& profile,
                                                      const PreviewOptions& options = {});

/// The media of `preview`'s games (Games' population of catalog::measureCoverage()) under the library's media root. The
/// media root's state decides nothing: a root out of reach makes every game's media unknown, which the counts say.
[[nodiscard]] Expected<catalog::CoverageCounts> previewCoverage(const LibraryPreview& preview);

/// The media root of the library `profile` describes: its override, else ES-DE's MediaDirectory (or its default) from
/// the settings. A root that is not known (the settings cannot be read and there is no override) has no path.
[[nodiscard]] esde::ResolvedRoot mediaRootOf(const catalog::LibraryProfile& profile);

} // namespace rv::library
