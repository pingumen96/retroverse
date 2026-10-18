#include <rv/app/library_refresh.hpp>

#include <algorithm>
#include <memory>
#include <utility>

#include <rv/catalog/change_set.hpp>

namespace rv::app {

std::string_view toString(RefreshPhase phase) noexcept {
    switch (phase) {
    case RefreshPhase::Reading:
        return "reading";
    case RefreshPhase::Media:
        return "media";
    case RefreshPhase::Comparing:
        return "comparing";
    case RefreshPhase::Writing:
        return "writing";
    }
    return "reading";
}

std::string_view toString(RefreshOutcome outcome) noexcept {
    switch (outcome) {
    case RefreshOutcome::Done:
        return "done";
    case RefreshOutcome::Cancelled:
        return "cancelled";
    case RefreshOutcome::Busy:
        return "busy";
    case RefreshOutcome::Failed:
        return "failed";
    }
    return "failed";
}

std::string_view toString(RefreshTrigger trigger) noexcept {
    switch (trigger) {
    case RefreshTrigger::CommandLine:
        return "command-line";
    case RefreshTrigger::User:
        return "user";
    }
    return "user";
}

RefreshSummary summarizeImport(const Expected<catalog::ImportResult>& result) {
    RefreshSummary summary;
    if (!result) {
        const Error& error = result.error();
        summary.error = error;
        if (error.code == ErrorCode::Cancelled) {
            summary.outcome = RefreshOutcome::Cancelled;
        } else if (error.code == ErrorCode::Conflict && error.context != "library.json") {
            // The import lock (another import runs), or a database another writer kept busy past its timeout.
            summary.outcome = RefreshOutcome::Busy;
        } else {
            summary.outcome = RefreshOutcome::Failed;
        }
        return summary;
    }
    const catalog::ChangeSet& changes = result->changes;
    summary.entries = result->statistics.entries;
    summary.added = changes.added.size();
    summary.removed = changes.removed.size();
    summary.changed = changes.metadataChanged.size();
    summary.moved = changes.moved.size();
    summary.reviews = changes.reviews.size();
    summary.guarded = static_cast<std::size_t>(
        std::count_if(changes.systems.begin(), changes.systems.end(), [](const catalog::SystemChanges& system) {
            return system.outcome == catalog::SystemOutcome::KeptByGuard;
        }));
    summary.firstImport = changes.firstImport;
    return summary;
}

namespace {

using Clock = std::chrono::steady_clock;

/// The slice boundaries of one import: when the worker looks at its requests, the progress it posts.
class Slicer {
public:
    Slicer(RefreshHost& host, RefreshSlicing& slicing) : m_host(host), m_slicing(slicing), m_last(Clock::now()) {}

    /// A port is called: a boundary, where the requests queued are answered once a slice has passed.
    void boundary() {
        const Clock::time_point now = Clock::now();
        if (now - m_last < kRefreshSlice) {
            return;
        }
        noteSlice(now);
        m_slicing.served += m_host.serve(m_writing);
        ++m_slicing.boundaries;
        m_last = Clock::now();
    }

    /// Runs `work` on the host's helper thread while it answers requests.
    void offload(const std::function<void()>& work) {
        noteSlice(Clock::now());
        m_slicing.served += m_host.serveWhile(work);
        ++m_slicing.boundaries;
        m_last = Clock::now();
    }

    /// The end of the import: its last stretch counts.
    void finish() { noteSlice(Clock::now()); }

    void writing(bool open) {
        m_writing = open;
        m_host.writing(open);
    }

    /// Sets the phase (posted at once when it changed) and its progress (posted every kRefreshProgressInterval).
    void progress(RefreshPhase phase, std::size_t done, std::size_t total) {
        const RefreshProgress progress{phase, done, total};
        const Clock::time_point now = Clock::now();
        const bool newPhase = !m_posted || m_posted->phase != phase;
        if (!newPhase && (progress == *m_posted || now - m_postedAt < kRefreshProgressInterval)) {
            m_current = progress;
            return;
        }
        m_current = progress;
        m_posted = progress;
        m_postedAt = now;
        m_host.progress(progress);
    }
    void step(RefreshPhase phase, std::size_t total) {
        const std::size_t done = m_current.phase == phase ? m_current.done + 1 : 1;
        progress(phase, done, std::max(total, done));
    }
    [[nodiscard]] const RefreshProgress& current() const noexcept { return m_current; }

    std::size_t entries = 0;
    std::size_t systems = 0;
    std::size_t rows = 0;

private:
    void noteSlice(Clock::time_point now) {
        m_slicing.longestSliceMs = std::max(
            m_slicing.longestSliceMs, std::chrono::duration<double, std::milli>(now - m_last).count());
    }

    RefreshHost& m_host;
    RefreshSlicing& m_slicing;
    Clock::time_point m_last;
    bool m_writing = false;
    RefreshProgress m_current;
    std::optional<RefreshProgress> m_posted;
    Clock::time_point m_postedAt{};
};

/// The library's load on the helper thread.
class SlicedSource final : public catalog::ILibrarySource {
public:
    SlicedSource(catalog::ILibrarySource& inner, Slicer& slicer) : m_inner(inner), m_slicer(slicer) {}

    Expected<catalog::LibrarySnapshot> load(const catalog::LibraryProfile& profile,
                                            const catalog::LibraryLoadOptions& options) override {
        m_slicer.progress(RefreshPhase::Reading, 0, 0);
        std::optional<Expected<catalog::LibrarySnapshot>> loaded;
        m_slicer.offload([&] { loaded.emplace(m_inner.load(profile, options)); });
        if (*loaded) {
            m_slicer.entries = (*loaded)->entries.size();
            m_slicer.systems = (*loaded)->systems.size();
        }
        return std::move(*loaded);
    }

private:
    catalog::ILibrarySource& m_inner;
    Slicer& m_slicer;
};

/// The media questions: the first of each system (its folders listed) on the helper thread, the others sliced.
class SlicedMedia final : public catalog::IMediaResolver {
public:
    SlicedMedia(catalog::IMediaResolver& inner, Slicer& slicer) : m_inner(inner), m_slicer(slicer) {}

    Expected<catalog::EntryMedia> presence(const catalog::SystemName& system, const catalog::LookupKey& key,
                                           bool directory) override {
        m_slicer.step(RefreshPhase::Media, m_slicer.entries);
        if (!m_system || *m_system != system) {
            m_system = system;
            std::optional<Expected<catalog::EntryMedia>> media;
            m_slicer.offload([&] { media.emplace(m_inner.presence(system, key, directory)); });
            return std::move(*media);
        }
        m_slicer.boundary();
        return m_inner.presence(system, key, directory);
    }
    Expected<std::filesystem::path> path(const catalog::SystemName& system, const catalog::LookupKey& key,
                                         bool directory, catalog::MediaKind kind,
                                         const catalog::MediaPresence& media) const override {
        return m_inner.path(system, key, directory, kind, media);
    }
    void refresh() noexcept override {
        m_system.reset();
        m_inner.refresh();
    }

private:
    catalog::IMediaResolver& m_inner;
    Slicer& m_slicer;
    std::optional<catalog::SystemName> m_system;
};

/// The pairing pass (the quick hashes of new files) on the helper thread.
class SlicedPairing final : public catalog::IIdentityPairing {
public:
    SlicedPairing(catalog::IIdentityPairing& inner, Slicer& slicer) : m_inner(inner), m_slicer(slicer) {}

    Expected<catalog::IdentityPairs> pair(const catalog::PairingRequest& request) override {
        std::optional<Expected<catalog::IdentityPairs>> pairs;
        m_slicer.offload([&] { pairs.emplace(m_inner.pair(request)); });
        return std::move(*pairs);
    }

private:
    catalog::IIdentityPairing& m_inner;
    Slicer& m_slicer;
};

/// The import's write transaction: a boundary before each write (only reads answered while it is open).
class SlicedTransaction final : public catalog::IImportTransaction {
public:
    SlicedTransaction(std::unique_ptr<catalog::IImportTransaction> inner, Slicer& slicer)
        : m_inner(std::move(inner)), m_slicer(slicer) {
        m_slicer.writing(true);
        m_slicer.progress(RefreshPhase::Writing, 0, 0);
    }
    ~SlicedTransaction() override {
        m_inner.reset(); // rolled back unless committed
        if (m_open) {
            m_slicer.writing(false);
        }
    }
    SlicedTransaction(const SlicedTransaction&) = delete;
    SlicedTransaction& operator=(const SlicedTransaction&) = delete;

    Status putLibrary(const catalog::LibraryProfile& profile) override {
        m_slicer.boundary();
        return m_inner->putLibrary(profile);
    }
    Status putIdentity(const catalog::StoredIdentity& identity) override {
        m_slicer.boundary();
        return m_inner->putIdentity(identity);
    }
    Status putSystem(const catalog::LibraryId& library, const catalog::SystemInfo& system) override {
        m_slicer.boundary();
        return m_inner->putSystem(library, system);
    }
    Status putEntry(const catalog::IndexedEntry& entry) override {
        m_slicer.step(RefreshPhase::Writing, m_slicer.rows);
        m_slicer.boundary();
        return m_inner->putEntry(entry);
    }
    Status removeEntry(catalog::GameId id) override {
        m_slicer.step(RefreshPhase::Writing, m_slicer.rows);
        m_slicer.boundary();
        return m_inner->removeEntry(id);
    }
    Expected<bool> prepareWrites(std::size_t rows) override {
        m_slicer.rows = rows;
        m_slicer.progress(RefreshPhase::Writing, 0, rows);
        m_slicer.boundary();
        return m_inner->prepareWrites(rows);
    }
    Status clearIndex(const catalog::LibraryId& library) override {
        m_slicer.boundary();
        return m_inner->clearIndex(library);
    }
    Status removeSystem(const catalog::LibraryId& library, const catalog::SystemName& system) override {
        m_slicer.boundary();
        return m_inner->removeSystem(library, system);
    }
    Status putDiscGroup(const catalog::IndexedDiscGroup& group) override {
        m_slicer.boundary();
        return m_inner->putDiscGroup(group);
    }
    Status removeDiscGroup(catalog::GameId head) override {
        m_slicer.boundary();
        return m_inner->removeDiscGroup(head);
    }
    Status putAlias(const catalog::IdentityAlias& alias) override {
        m_slicer.boundary();
        return m_inner->putAlias(alias);
    }
    Status putQuickHash(catalog::GameId id, const catalog::QuickHash& hash) override {
        m_slicer.boundary();
        return m_inner->putQuickHash(id, hash);
    }
    Status putHistory(const catalog::IdentityEvent& event) override {
        m_slicer.boundary();
        return m_inner->putHistory(event);
    }
    Expected<std::int64_t> putReview(const catalog::ReviewRow& review) override {
        m_slicer.boundary();
        return m_inner->putReview(review);
    }
    Status resolveReview(std::int64_t id, catalog::ReviewResolution resolution,
                         std::optional<catalog::GameId> with) override {
        m_slicer.boundary();
        return m_inner->resolveReview(id, resolution, with);
    }
    Status resolveReviewsOf(catalog::GameId game, catalog::ReviewResolution resolution) override {
        m_slicer.boundary();
        return m_inner->resolveReviewsOf(game, resolution);
    }
    Status putImportIssues(const catalog::LibraryId& library, std::span<const catalog::Issue> issues,
                           std::span<const catalog::SystemName> systems) override {
        m_slicer.boundary();
        return m_inner->putImportIssues(library, issues, systems);
    }
    Status recordImport(const catalog::ImportRecord& record) override {
        m_slicer.boundary();
        return m_inner->recordImport(record);
    }
    Status commit() override {
        m_slicer.boundary();
        Status committed = m_inner->commit();
        // The transaction ended either way (a COMMIT that fails rolls back).
        if (m_open) {
            m_open = false;
            m_slicer.writing(false);
        }
        return committed;
    }

private:
    std::unique_ptr<catalog::IImportTransaction> m_inner;
    Slicer& m_slicer;
    bool m_open = true;
};

/// The store: a boundary before each call (outside the write transaction everything is answered).
class SlicedStore final : public catalog::ICatalogStore {
public:
    SlicedStore(catalog::ICatalogStore& inner, Slicer& slicer) : m_inner(inner), m_slicer(slicer) {}

    Expected<std::vector<catalog::LibraryProfile>> libraries() override {
        m_slicer.boundary();
        return m_inner.libraries();
    }
    Expected<std::vector<catalog::StoredIdentity>> identities(const catalog::LibraryId& library,
                                                              const catalog::SystemName& system) override {
        // One system compared after another: identities first, then its rows.
        m_slicer.step(RefreshPhase::Comparing, m_slicer.systems);
        m_slicer.boundary();
        return m_inner.identities(library, system);
    }
    Expected<std::optional<catalog::StoredIdentity>> identity(catalog::GameId id) override {
        m_slicer.boundary();
        return m_inner.identity(id);
    }
    Expected<std::vector<catalog::GameId>> takenGameIds() override {
        m_slicer.boundary();
        return m_inner.takenGameIds();
    }
    Expected<std::vector<catalog::IndexedEntry>> indexedEntries(const catalog::LibraryId& library,
                                                                const catalog::SystemName& system) override {
        m_slicer.boundary();
        return m_inner.indexedEntries(library, system);
    }
    Expected<std::vector<catalog::SystemInfo>> systems(const catalog::LibraryId& library) override {
        m_slicer.boundary();
        return m_inner.systems(library);
    }
    Expected<std::vector<catalog::SystemName>> identitySystems(const catalog::LibraryId& library) override {
        m_slicer.boundary();
        return m_inner.identitySystems(library);
    }
    Expected<std::vector<catalog::IndexedDiscGroup>> discGroups(const catalog::LibraryId& library) override {
        m_slicer.boundary();
        return m_inner.discGroups(library);
    }
    Expected<std::optional<catalog::ImportRecord>> lastImport(const catalog::LibraryId& library) override {
        m_slicer.boundary();
        return m_inner.lastImport(library);
    }
    Expected<std::vector<catalog::IdentityEvent>> history(catalog::GameId game) override {
        m_slicer.boundary();
        return m_inner.history(game);
    }
    Expected<std::vector<catalog::StoredAlias>> aliasesOf(catalog::GameId game) override {
        m_slicer.boundary();
        return m_inner.aliasesOf(game);
    }
    Expected<std::optional<catalog::StoredAlias>> alias(catalog::GameId alias) override {
        m_slicer.boundary();
        return m_inner.alias(alias);
    }
    Expected<std::vector<catalog::StoredReview>> reviews(const catalog::LibraryId& library, bool openOnly) override {
        m_slicer.boundary();
        return m_inner.reviews(library, openOnly);
    }
    Expected<std::optional<catalog::StoredReview>> review(std::int64_t id) override {
        m_slicer.boundary();
        return m_inner.review(id);
    }
    Expected<std::vector<catalog::StoredReview>> reviewsOf(catalog::GameId game) override {
        m_slicer.boundary();
        return m_inner.reviewsOf(game);
    }
    Expected<std::unique_ptr<catalog::IImportLock>> lockImport(const catalog::LibraryId& library) override {
        m_slicer.boundary();
        return m_inner.lockImport(library);
    }
    Expected<std::unique_ptr<catalog::IImportTransaction>> beginImport(const catalog::LibraryId& library) override {
        m_slicer.boundary();
        auto transaction = m_inner.beginImport(library);
        if (!transaction) {
            return propagate(transaction.error());
        }
        return std::unique_ptr<catalog::IImportTransaction>(
            std::make_unique<SlicedTransaction>(std::move(*transaction), m_slicer));
    }

private:
    catalog::ICatalogStore& m_inner;
    Slicer& m_slicer;
};

} // namespace

Expected<catalog::ImportResult> runSlicedImport(const RefreshPorts& ports, const catalog::LibraryProfile& profile,
                                                const std::atomic<bool>& cancel, RefreshHost& host,
                                                RefreshSlicing& slicing) {
    slicing = RefreshSlicing{};
    Slicer slicer(host, slicing);
    SlicedSource source(ports.source, slicer);
    SlicedMedia media(ports.media, slicer);
    SlicedStore store(ports.store, slicer);
    std::optional<SlicedPairing> pairing;
    if (ports.pairing != nullptr) {
        pairing.emplace(*ports.pairing, slicer);
    }
    catalog::ImportService service(source, media, store, pairing ? &*pairing : nullptr);
    catalog::ImportOptions options;
    options.cancel = &cancel;
    auto result = service.run(profile, options);
    slicer.finish();
    return result;
}

} // namespace rv::app
