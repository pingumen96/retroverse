#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The catalog's refresh on its worker (I3 step 13r; ADR-0008 §5, docs/i2/step-13.md "Notes for step 13r"): the import
// of retroverse-cli import (catalog::ImportService with catalog::IdentityPairing) run as a job of the catalog worker,
// in slices, so that the quick catalog's lists, the launch readiness of its rows, the resolution of a launch and the
// play sessions never wait for the whole import. ImportService::run() is one call; runSlicedImport() slices it through
// its ports, which it wraps:
//  - every call of a port is a slice boundary: when kRefreshSlice has passed since the worker last looked, it answers
//    what the main thread queued meanwhile (RefreshHost::serve), on its own thread and connections, between two pieces
//    of the import's work;
//  - the long reads that touch no database run on a helper thread while the worker answers requests as they come
//    (RefreshHost::serveWhile): the library's load (ILibrarySource::load: ES-DE's configuration, the ROM folders, the
//    gamelists), the first media question of each system (the resolver lists its media folders then) and the pairing
//    pass (IIdentityPairing::pair: the quick hashes of new files);
//  - while the import's write transaction is open on the writer (between ICatalogStore::beginImport() and its commit
//    or rollback), only reads are answered (lists on the worker's reader, readiness); launches and session writes
//    wait for its end, as other writers of the database must (I2 step 11a: 0.05 s for a refresh that changes little).
// The import's cancel flag (ImportOptions::cancel, the load's too) is the worker's: CatalogWorker::stop() sets it, and
// interrupts the writer while the import writes, so that quitting during a refresh ends within its bound with the
// import rolled back or committed whole. Slices are statistics of the refresh (RefreshSlicing: the longest stretch
// without a boundary is what a list may have waited, beside its own time).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/library.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/media_resolver.hpp>
#include <rv/core/expected.hpp>

namespace rv::app {

/// The longest the worker runs the import before it looks at the main thread's requests again (a slice), when the
/// import's own pieces of work allow (a port call is the smallest piece).
inline constexpr std::chrono::milliseconds kRefreshSlice{4};
/// How often the refresh's progress is posted to the main thread at most (a new phase is posted at once).
inline constexpr std::chrono::milliseconds kRefreshProgressInterval{100};

/// Where a refresh is (ui::quick_catalog::kRefreshPhases names them for the document).
enum class RefreshPhase : std::uint8_t {
    Reading,   ///< ES-DE's configuration, the ROM folders and the gamelists (ILibrarySource::load)
    Media,     ///< the media of every entry (IMediaResolver::presence): done of total entries
    Comparing, ///< the catalog's identities and rows against the snapshot, the pairing pass: done of total systems
    Writing,   ///< the write transaction: done of total rows
};
/// "reading", "media", "comparing", "writing".
[[nodiscard]] std::string_view toString(RefreshPhase phase) noexcept;

/// How a refresh ended (ui::quick_catalog::kRefreshStates names them, with "running").
enum class RefreshOutcome : std::uint8_t {
    Done,      ///< imported (committed, or nothing to write)
    Cancelled, ///< stopped (the worker's stop: quitting): nothing written
    Busy,      ///< another import holds the import lock (retroverse-cli import, in this or another process)
    Failed,    ///< the library or the database cannot be read or written
};
/// "done", "cancelled", "busy", "failed".
[[nodiscard]] std::string_view toString(RefreshOutcome outcome) noexcept;

/// What made a refresh start (the run report's RefreshRecord::trigger).
enum class RefreshTrigger : std::uint8_t {
    CommandLine, ///< --refresh-library, once the catalog opened
    User,        ///< "Aggiorna libreria" in the quick catalog
    Wizard,      ///< the wizard's first import (ADR-0018 §4)
    Watcher,     ///< the file watcher's changes (ADR-0018 §10)
};
/// "command-line", "user", "wizard", "watcher".
[[nodiscard]] std::string_view toString(RefreshTrigger trigger) noexcept;

/// What a refresh imports (ADR-0018 §10).
struct RefreshRequest {
    RefreshTrigger trigger = RefreshTrigger::User;
    /// Only these systems: a partial refresh, the library's load and the import narrowed together
    /// (EsdeLibrarySourceOptions::systems with ImportOptions::systems), the other systems' rows left as they are. Empty:
    /// every system. A system the configuration no longer defines makes it a refresh of every system.
    std::vector<catalog::SystemName> systems;
    /// Apply the removals the mass-disappearance guard holds back: never for the watcher's (FR-033).
    bool acceptRemovals = false;

    bool operator==(const RefreshRequest&) const = default;
};

struct RefreshProgress {
    RefreshPhase phase = RefreshPhase::Reading;
    /// Media: entries asked, of the snapshot's; comparing: systems compared, of the snapshot's; writing: rows written,
    /// of those the import said it writes (0 while unknown).
    std::size_t done = 0;
    std::size_t total = 0;

    bool operator==(const RefreshProgress&) const = default;
};

/// How the import was sliced.
struct RefreshSlicing {
    /// Slice boundaries at which the worker looked at its requests, and the requests it answered there.
    std::uint64_t boundaries = 0;
    std::uint64_t served = 0;
    /// The longest stretch of the import on the worker's thread between two looks at its requests, in milliseconds:
    /// what a request queued meanwhile waited at most before the worker took it.
    double longestSliceMs = 0.0;

    bool operator==(const RefreshSlicing&) const = default;
};

/// What a refresh did (CatalogWorker's CatalogRefreshed; the run report's RefreshRecord).
struct RefreshSummary {
    RefreshOutcome outcome = RefreshOutcome::Done;
    /// Why it did not end Done.
    std::optional<Error> error;
    /// The snapshot's entries, and what the import changed (catalog::ChangeSet): games added to and removed from the
    /// index, rows written again (metadata, availability, media), identities that followed their file, review rows;
    /// systems the mass-disappearance guard kept as they were (retroverse-cli import --accept-removals applies them).
    std::size_t entries = 0;
    std::size_t added = 0;
    std::size_t removed = 0;
    std::size_t changed = 0;
    std::size_t moved = 0;
    std::size_t reviews = 0;
    std::size_t guarded = 0;
    /// The library's index held no row before.
    bool firstImport = false;
    /// On the worker, from the job's start to its result (the systems' readiness resolved again included).
    double milliseconds = 0.0;
    RefreshSlicing slicing;

    bool operator==(const RefreshSummary&) const = default;
};

/// The summary of an import's result (the outcome from its error: Cancelled, Conflict of the import lock as Busy, the
/// rest Failed); slicing and time left for the caller.
[[nodiscard]] RefreshSummary summarizeImport(const Expected<catalog::ImportResult>& result);

/// What a sliced import asks of the catalog worker, on the worker's thread.
class RefreshHost {
public:
    virtual ~RefreshHost() = default;
    /// A slice boundary: answers the requests queued. `writing`: the import's write transaction is open on the
    /// writer, so only reads are answered (lists, readiness). Returns how many it answered.
    virtual std::size_t serve(bool writing) = 0;
    /// Runs `work` on a helper thread (it touches no database connection) and answers the requests that come
    /// meanwhile (as serve(false)) until it returned; an exception it throws is thrown again here. Returns how many
    /// requests it answered.
    virtual std::size_t serveWhile(const std::function<void()>& work) = 0;
    /// Where the refresh is (posted to the main thread).
    virtual void progress(const RefreshProgress& progress) = 0;
    /// The import's write transaction began (true) or ended (false).
    virtual void writing(bool open) = 0;
};

/// The ports of one import, which must outlive the call.
struct RefreshPorts {
    catalog::ILibrarySource& source;
    catalog::IMediaResolver& media;
    catalog::ICatalogStore& store;
    /// Null: no reconciliation.
    catalog::IIdentityPairing* pairing = nullptr;
};

/// Runs catalog::ImportService::run(profile, options) over `ports` in slices (above), on the calling thread (the
/// worker's), with `cancel` as its cancel flag (over the options'); `slicing` says how it was sliced. The import's errors
/// as ImportService gives them.
[[nodiscard]] Expected<catalog::ImportResult> runSlicedImport(const RefreshPorts& ports,
                                                              const catalog::LibraryProfile& profile,
                                                              const std::atomic<bool>& cancel, RefreshHost& host,
                                                              RefreshSlicing& slicing,
                                                              catalog::ImportOptions options = {});

} // namespace rv::app
