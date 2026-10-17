#include <rv/catalog/import_service.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/identity.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/time.hpp>

namespace rv::catalog {
namespace {

using Clock = std::chrono::steady_clock;

/// How many writes go between two looks at the cancellation flag.
constexpr std::size_t kWritesBetweenCancelChecks = 256;

bool isCancelled(const std::atomic<bool>* cancel) noexcept {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

tl::unexpected<Error> cancelledError() {
    return makeError(ErrorCode::Cancelled, "the import was cancelled: nothing was written");
}

Error withPrefix(Error error, std::string_view prefix) {
    error.message = fmt::format("{}: {}", prefix, error.message);
    return error;
}

/// The folders of a system that could not be listed, by key: nothing is known below them (ADR-0008 §4).
class UnavailableFolders {
public:
    /// The system folder itself: everything below it.
    void markAll(SourceUnavailableReason reason) { m_all = reason; }
    /// A folder below the system folder, by its key.
    void add(std::string key, SourceUnavailableReason reason) { m_folders.emplace_back(std::move(key), reason); }

    /// The reason the file or folder of `key` cannot be known, or nothing when its folder was listed.
    [[nodiscard]] std::optional<SourceUnavailableReason> reasonFor(std::string_view key) const {
        if (m_all) {
            return m_all;
        }
        for (const auto& [folder, reason] : m_folders) {
            if (key.size() >= folder.size() && key.substr(0, folder.size()) == folder &&
                (key.size() == folder.size() || key[folder.size()] == '/')) {
                return reason;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] bool any() const noexcept { return m_all.has_value() || !m_folders.empty(); }

private:
    std::optional<SourceUnavailableReason> m_all;
    std::vector<std::pair<std::string, SourceUnavailableReason>> m_folders;
};

/// No identity, no entry.
constexpr std::ptrdiff_t kNone = -1;

/// One system of the import: what the store holds, what the snapshot gives, how they match, what is written.
struct SystemPlan {
    SystemName name;
    /// The snapshot's system; null when the configuration no longer defines it.
    const SystemInfo* info = nullptr;
    /// Its row in the index, if any.
    std::optional<SystemInfo> storedInfo;
    std::vector<StoredIdentity> identities; ///< ICatalogStore::identities() order
    std::vector<IndexedEntry> rows;         ///< entryOrder()
    std::vector<IndexedDiscGroup> groups;   ///< the stored groups of the system, by head
    std::vector<std::size_t> entries;       ///< the snapshot's entries of the system with a key, entryOrder()
    UnavailableFolders unavailable;

    std::vector<std::ptrdiff_t> entryIdentity; ///< per entries[i]: an index into identities, or kNone (a new key)
    std::vector<std::ptrdiff_t> identityEntry; ///< per identity: an index into entries, or kNone
    std::vector<std::optional<MoveReason>> moved; ///< per identity: paired with a new file
    std::vector<GameId> entryIds;              ///< per entries[i], once given out

    /// The system's changes are written (false: kept as it was).
    bool apply = true;
    SystemChanges changes;
};

/// The writes of the import, collected before any is made, so that each kind is written in ascending GameId order.
struct Writes {
    std::vector<SystemInfo> systems;
    std::vector<SystemName> removedSystems;
    std::vector<StoredIdentity> identitiesBefore; ///< identities that exist: written first
    std::vector<StoredIdentity> identitiesNew;
    std::vector<IndexedEntry> rows;
    std::vector<GameId> removals;
    std::vector<IndexedDiscGroup> groups;
    std::vector<GameId> groupRemovals;
};

/// One run of ImportService.
class ImportRun {
public:
    ImportRun(ILibrarySource& source, IMediaResolver& media, ICatalogStore& store, IIdentityPairing* pairing,
              const LibraryProfile& profile, const ImportOptions& options)
        : m_source(source), m_media(media), m_store(store), m_pairing(pairing), m_profile(profile),
          m_options(options) {}

    Expected<ImportResult> run();

private:
    [[nodiscard]] bool cancelled() const noexcept { return isCancelled(m_options.cancel); }
    [[nodiscard]] bool inScope(const SystemName& name) const {
        return m_options.systems.empty() ||
               std::find(m_options.systems.begin(), m_options.systems.end(), name) != m_options.systems.end();
    }

    Status checkLibrary();
    Status askMedia();
    Expected<SystemPlan> readSystem(const SystemName& name, const std::vector<SystemInfo>& storedSystems);
    Status match(SystemPlan& plan);
    void keepAsItWas(SystemPlan& plan, SystemOutcome outcome);
    Status assignIds();
    Status buildWrites(SystemPlan& plan);
    void buildIdentityWrites(SystemPlan& plan);
    Status buildRowWrites(SystemPlan& plan);
    void buildGroupWrites(SystemPlan& plan);
    void addIssue(IssueSeverity severity, std::string_view code, const SystemName& system, std::string reason);
    Status write();

    ILibrarySource& m_source;
    IMediaResolver& m_media;
    ICatalogStore& m_store;
    IIdentityPairing* m_pairing;
    const LibraryProfile& m_profile;
    const ImportOptions& m_options;

    LookupKeyVersion m_keyVersion;
    std::vector<IndexedDiscGroup> m_storedGroups;
    LibrarySnapshot m_snapshot;
    /// Per snapshot entry: its media, nothing when presence() could not know them.
    std::vector<std::optional<EntryMedia>> m_entryMedia;
    std::vector<GameId> m_taken; ///< ascending
    /// Declared before the transaction: a transaction left open rolls back before the import lock goes.
    std::unique_ptr<IImportLock> m_lock;
    std::unique_ptr<IImportTransaction> m_transaction;
    std::vector<SystemPlan> m_plans;
    /// The import's own issues, in the order they were met.
    std::vector<Issue> m_issues;
    Writes m_writes;
    ImportResult m_result;
    std::string m_startedAt;
};

Status ImportRun::checkLibrary() {
    auto libraries = m_store.libraries();
    if (!libraries) {
        return propagate(libraries.error());
    }
    if (libraries->empty() ||
        std::any_of(libraries->begin(), libraries->end(),
                    [&](const LibraryProfile& library) { return library.id == m_profile.id; })) {
        return ok();
    }
    std::string held;
    for (const LibraryProfile& library : *libraries) {
        held += fmt::format("{}'{}'", held.empty() ? "" : ", ", library.id.str());
    }
    return makeError(ErrorCode::Conflict,
                     fmt::format("library.json names the library '{}', the catalog holds {}: the catalog is the "
                                 "authority on the library's id, from which every GameId derives; name the catalog's "
                                 "library in library.json again to import into it",
                                 m_profile.id.str(), held),
                     "library.json");
}

Status ImportRun::askMedia() {
    m_entryMedia.assign(m_snapshot.entries.size(), std::nullopt);
    for (std::size_t i = 0; i < m_snapshot.entries.size(); ++i) {
        if (i % kWritesBetweenCancelChecks == 0 && cancelled()) {
            return cancelledError();
        }
        const CatalogEntry& entry = m_snapshot.entries[i];
        if (entry.key.empty() || !inScope(entry.system)) {
            continue;
        }
        const bool directory = entry.file ? entry.file->directory : entry.state == EntryState::Folder;
        auto media = m_media.presence(entry.system, entry.key, directory);
        if (media) {
            m_entryMedia[i] = std::move(*media);
        } else if (media.error().code == ErrorCode::Unavailable) {
            ++m_result.statistics.mediaUnknown; // what the row held is kept
        } else {
            return propagate(withPrefix(media.error(), "cannot know the media of " + entry.system.str() + "/" +
                                                           entry.key.relativePath()));
        }
    }
    return ok();
}

Expected<SystemPlan> ImportRun::readSystem(const SystemName& name, const std::vector<SystemInfo>& storedSystems) {
    SystemPlan plan;
    plan.name = name;
    plan.changes.system = name;
    plan.info = m_snapshot.findSystem(name);
    for (const SystemInfo& stored : storedSystems) {
        if (stored.name == name) {
            plan.storedInfo = stored;
        }
    }
    auto identities = m_store.identities(m_profile.id, name);
    if (!identities) {
        return propagate(identities.error());
    }
    plan.identities = std::move(*identities);
    auto rows = m_store.indexedEntries(m_profile.id, name);
    if (!rows) {
        return propagate(rows.error());
    }
    plan.rows = std::move(*rows);
    for (const IndexedDiscGroup& group : m_storedGroups) {
        if (group.cached.group.system == name) {
            plan.groups.push_back(group);
        }
    }
    for (std::size_t i = 0; i < m_snapshot.entries.size(); ++i) {
        const CatalogEntry& entry = m_snapshot.entries[i];
        if (entry.system == name && !entry.key.empty()) {
            plan.entries.push_back(i);
        }
    }
    // The snapshot holds each system's entries in entryOrder(), the import relies on it.
    std::sort(plan.entries.begin(), plan.entries.end(), [&](std::size_t a, std::size_t b) {
        return entryOrder(m_snapshot.entries[a], m_snapshot.entries[b]);
    });
    if (plan.info != nullptr) {
        if (plan.info->availability != Availability::Available) {
            plan.unavailable.markAll(plan.info->unavailableReason);
        }
        for (const UnavailableFolder& folder : m_snapshot.unavailableFolders) {
            if (folder.system != name) {
                continue;
            }
            auto key = makeLookupKey(folder.relativePath);
            if (key) {
                plan.unavailable.add(key->key(), folder.reason);
            }
        }
    }
    return plan;
}

void ImportRun::addIssue(IssueSeverity severity, std::string_view code, const SystemName& system, std::string reason) {
    m_issues.push_back(Issue{severity, std::string(code), system.str(), {}, std::move(reason)});
}

void ImportRun::keepAsItWas(SystemPlan& plan, SystemOutcome outcome) {
    plan.apply = false;
    plan.changes.outcome = outcome;
    plan.changes.kept = plan.rows.size();
}

Status ImportRun::match(SystemPlan& plan) {
    const std::vector<StoredIdentity>& identities = plan.identities;
    const std::size_t entryCount = plan.entries.size();
    plan.entryIdentity.assign(entryCount, kNone);
    plan.identityEntry.assign(identities.size(), kNone);
    plan.moved.assign(identities.size(), std::nullopt);
    const auto entryAt = [&](std::size_t i) -> const CatalogEntry& { return m_snapshot.entries[plan.entries[i]]; };

    // The exact stored relative path first (a file has one identity: paths are unique per system) ...
    std::unordered_map<std::string_view, std::size_t> byPath;
    byPath.reserve(identities.size());
    for (std::size_t j = 0; j < identities.size(); ++j) {
        byPath.emplace(identities[j].key.relativePath(), j);
    }
    for (std::size_t i = 0; i < entryCount; ++i) {
        const auto found = byPath.find(entryAt(i).key.relativePath());
        if (found != byPath.end()) {
            plan.entryIdentity[i] = static_cast<std::ptrdiff_t>(found->second);
            plan.identityEntry[found->second] = static_cast<std::ptrdiff_t>(i);
        }
    }
    // ... then the key: the identities are in key order, an entry takes the first unmatched one of its key (a file
    // renamed in case only, or a key computed with another casing table), names that fold to one key in order.
    for (std::size_t i = 0; i < entryCount; ++i) {
        if (plan.entryIdentity[i] != kNone) {
            continue;
        }
        const std::string& key = entryAt(i).key.key();
        auto it = std::lower_bound(identities.begin(), identities.end(), key,
                                   [](const StoredIdentity& identity, const std::string& k) { return identity.key.key() < k; });
        for (; it != identities.end() && it->key.key() == key; ++it) {
            const auto j = static_cast<std::size_t>(it - identities.begin());
            if (plan.identityEntry[j] == kNone) {
                plan.entryIdentity[i] = static_cast<std::ptrdiff_t>(j);
                plan.identityEntry[j] = static_cast<std::ptrdiff_t>(i);
                break;
            }
        }
    }
    // Names that fold to one key keep identities of their own (ADR-0008 §4): reported once per key.
    for (std::size_t i = 1; i < entryCount; ++i) {
        const LookupKey& previous = entryAt(i - 1).key;
        const LookupKey& current = entryAt(i).key;
        if (previous.key() == current.key() && (i < 2 || entryAt(i - 2).key.key() != current.key())) {
            addIssue(IssueSeverity::Info, kImportIssueKeyCollision, plan.name,
                     fmt::format("\"{}\" and \"{}\" fold to one lookup key: each keeps an identity of its own",
                                 previous.relativePath(), current.relativePath()));
        }
    }

    // The identities whose file vanished (present at the last import, gone from a folder that was listed), and the
    // new files: what the pairing pass (I2 step 11b) may pair.
    std::vector<VanishedIdentity> vanished;
    std::vector<std::size_t> vanishedIndex;
    for (std::size_t j = 0; j < identities.size(); ++j) {
        const StoredIdentity& identity = identities[j];
        if (!identity.present || plan.unavailable.reasonFor(identity.key.key())) {
            continue; // not present at the last import, or nothing is known about it now
        }
        ++plan.changes.presentBefore;
        const std::ptrdiff_t matched = plan.identityEntry[j];
        if (matched == kNone) {
            vanished.push_back(VanishedIdentity{identity, false});
            vanishedIndex.push_back(j);
            continue;
        }
        const CatalogEntry& entry = entryAt(static_cast<std::size_t>(matched));
        if (!entry.file && entry.availability == Availability::Missing) {
            vanished.push_back(VanishedIdentity{identity, true});
            vanishedIndex.push_back(j);
        }
    }
    std::vector<NewFile> newFiles;
    std::vector<std::size_t> newFileIndex;
    for (std::size_t i = 0; i < entryCount; ++i) {
        const CatalogEntry& entry = entryAt(i);
        if (plan.entryIdentity[i] == kNone && entry.file && entry.availability == Availability::Available) {
            newFiles.push_back(NewFile{entry.key, *entry.file, entry.state});
            newFileIndex.push_back(i);
        }
    }

    std::size_t paired = 0;
    if (m_pairing != nullptr && plan.info != nullptr && !vanished.empty() && !newFiles.empty()) {
        auto pairs = m_pairing->pair(PairingRequest{m_profile.id, *plan.info, vanished, newFiles, m_options.cancel});
        if (!pairs) {
            return propagate(pairs.error());
        }
        if (cancelled()) {
            return cancelledError();
        }
        std::vector<bool> identityUsed(vanished.size(), false);
        std::vector<bool> fileUsed(newFiles.size(), false);
        for (const IdentityPair& pair : pairs->pairs) {
            const auto v = std::find_if(vanished.begin(), vanished.end(),
                                        [&](const VanishedIdentity& candidate) { return candidate.identity.id == pair.identity; });
            const auto f = std::find_if(newFiles.begin(), newFiles.end(),
                                        [&](const NewFile& candidate) { return candidate.key == pair.newFile; });
            if (v == vanished.end() || f == newFiles.end() || identityUsed[v - vanished.begin()] ||
                fileUsed[f - newFiles.begin()]) {
                return makeError(ErrorCode::InvalidArgument,
                                 "the pairing pass paired an identity or a file it was not given, or one twice",
                                 pair.identity.toString() + " " + pair.newFile.relativePath());
            }
            identityUsed[v - vanished.begin()] = true;
            fileUsed[f - newFiles.begin()] = true;
            const std::size_t j = vanishedIndex[static_cast<std::size_t>(v - vanished.begin())];
            const std::size_t i = newFileIndex[static_cast<std::size_t>(f - newFiles.begin())];
            // A MissingFile entry left at the old path loses the identity: it gets a new GameId.
            if (const std::ptrdiff_t old = plan.identityEntry[j]; old != kNone) {
                plan.entryIdentity[static_cast<std::size_t>(old)] = kNone;
            }
            plan.identityEntry[j] = static_cast<std::ptrdiff_t>(i);
            plan.entryIdentity[i] = static_cast<std::ptrdiff_t>(j);
            plan.moved[j] = pair.reason;
            m_result.changes.moved.push_back(
                MovedGame{identities[j].id, plan.name, identities[j].key, entryAt(i).key, pair.reason});
            ++paired;
        }
        for (ReviewRow& review : pairs->reviews) {
            m_result.changes.reviews.push_back(std::move(review));
        }
        for (Issue& issue : pairs->issues) {
            m_issues.push_back(std::move(issue));
        }
    }
    plan.changes.moved = paired;
    plan.changes.gone = vanished.size() - paired;
    return ok();
}

Status ImportRun::assignIds() {
    std::vector<GameIdRequest> requests;
    std::vector<std::pair<std::size_t, std::size_t>> owners; // (plan, entry index)
    for (std::size_t p = 0; p < m_plans.size(); ++p) {
        SystemPlan& plan = m_plans[p];
        plan.entryIds.assign(plan.entries.size(), GameId{});
        if (!plan.apply) {
            continue;
        }
        for (std::size_t i = 0; i < plan.entries.size(); ++i) {
            if (plan.entryIdentity[i] != kNone) {
                plan.entryIds[i] = plan.identities[static_cast<std::size_t>(plan.entryIdentity[i])].id;
                continue;
            }
            requests.push_back(GameIdRequest{plan.name, m_snapshot.entries[plan.entries[i]].key});
            owners.emplace_back(p, i);
        }
    }
    const auto taken = [&](GameId id) {
        return std::binary_search(m_taken.begin(), m_taken.end(), id);
    };
    auto assigned = assignGameIds(m_profile.id, requests, taken);
    if (!assigned) {
        return propagate(assigned.error());
    }
    for (std::size_t r = 0; r < requests.size(); ++r) {
        SystemPlan& plan = m_plans[owners[r].first];
        plan.entryIds[owners[r].second] = (*assigned)[r].id;
        ++plan.changes.newIdentities;
    }
    return ok();
}

void ImportRun::buildIdentityWrites(SystemPlan& plan) {
    for (std::size_t j = 0; j < plan.identities.size(); ++j) {
        const StoredIdentity& stored = plan.identities[j];
        StoredIdentity identity = stored;
        const std::ptrdiff_t matched = plan.identityEntry[j];
        const bool unknown = plan.unavailable.reasonFor(stored.key.key()).has_value();
        if (matched != kNone) {
            const CatalogEntry& entry = m_snapshot.entries[plan.entries[static_cast<std::size_t>(matched)]];
            if (entry.file && entry.availability == Availability::Available) {
                identity.key = entry.key; // seen: the path as listed now
                identity.present = true;
                identity.file = entry.file;
            } else if (entry.availability != Availability::SourceUnavailable) {
                identity.present = false; // its file gone, its gamelist entry left: last path and facts kept
            }
        } else if (!unknown) {
            identity.present = false; // vanished, or left the snapshot: last path and facts kept
        }
        identity.keyVersion = m_keyVersion; // stored keys of another version are computed again, in place
        plan.changes.rekeyed += stored.keyVersion != m_keyVersion ? 1 : 0;
        plan.changes.vanished += stored.present && !identity.present ? 1 : 0;
        plan.changes.returned += !stored.present && identity.present ? 1 : 0;
        if (identity != stored) {
            m_writes.identitiesBefore.push_back(std::move(identity));
        }
    }
    for (std::size_t i = 0; i < plan.entries.size(); ++i) {
        if (plan.entryIdentity[i] != kNone) {
            continue;
        }
        const CatalogEntry& entry = m_snapshot.entries[plan.entries[i]];
        StoredIdentity identity;
        identity.id = plan.entryIds[i];
        identity.library = m_profile.id;
        identity.system = plan.name;
        identity.key = entry.key;
        identity.keyVersion = m_keyVersion;
        identity.present = entry.file.has_value() && entry.availability == Availability::Available;
        identity.file = identity.present ? entry.file : std::nullopt;
        m_writes.identitiesNew.push_back(std::move(identity));
    }
}

Status ImportRun::buildRowWrites(SystemPlan& plan) {
    std::unordered_map<GameId, std::size_t> rowById;
    rowById.reserve(plan.rows.size());
    for (std::size_t r = 0; r < plan.rows.size(); ++r) {
        rowById.emplace(plan.rows[r].id, r);
    }
    std::vector<bool> covered(plan.rows.size(), false);
    SystemChanges& changes = plan.changes;
    ChangeSet& changeSet = m_result.changes;

    const auto record = [&](const IndexedEntry& row, const IndexedEntry* stored) {
        const ChangedGame game{row.id, plan.name, row.entry.key};
        if (stored == nullptr) {
            changeSet.added.push_back(game);
        } else if (!(row == *stored)) {
            changeSet.metadataChanged.push_back(game);
            if (row.entry.availability != stored->entry.availability) {
                changeSet.availabilityChanged.push_back(AvailabilityChange{
                    row.id, plan.name, row.entry.key, stored->entry.availability, row.entry.availability});
            }
        }
        if (m_options.rebuildIndex || stored == nullptr || !(row == *stored)) {
            m_writes.rows.push_back(row);
        }
    };

    for (std::size_t i = 0; i < plan.entries.size(); ++i) {
        const std::size_t index = plan.entries[i];
        const CatalogEntry& entry = m_snapshot.entries[index];
        const GameId id = plan.entryIds[i];
        const auto found = rowById.find(id);
        const IndexedEntry* stored = found == rowById.end() ? nullptr : &plan.rows[found->second];
        if (stored != nullptr) {
            covered[found->second] = true;
        }
        IndexedEntry row;
        if (entry.availability == Availability::SourceUnavailable && stored != nullptr) {
            // Under a folder that could not be listed: the row kept as it was, its availability saying why.
            row = *stored;
            row.entry.availability = Availability::SourceUnavailable;
            row.entry.unavailableReason = entry.unavailableReason;
        } else {
            row.id = id;
            row.entry = entry;
            if (m_entryMedia[index]) {
                row.media = *m_entryMedia[index];
            } else if (stored != nullptr) {
                row.media = stored->media; // presence() could not know: what was stored is kept
            }
            auto sortKey = sortKeyOf(entry);
            if (!sortKey) {
                return propagate(withPrefix(sortKey.error(), "cannot sort " + plan.name.str() + "/" +
                                                                 entry.key.relativePath()));
            }
            row.sortKey = std::move(*sortKey);
        }
        if (entry.availability == Availability::SourceUnavailable) {
            ++changes.unavailable;
        } else if (stored == nullptr) {
            ++changes.added;
        } else if (row == *stored) {
            ++changes.unchanged;
        } else {
            ++changes.changed;
        }
        record(row, stored);
    }
    for (std::size_t r = 0; r < plan.rows.size(); ++r) {
        if (covered[r]) {
            continue;
        }
        const IndexedEntry& stored = plan.rows[r];
        if (const auto reason = plan.unavailable.reasonFor(stored.entry.key.key())) {
            IndexedEntry row = stored;
            row.entry.availability = Availability::SourceUnavailable;
            row.entry.unavailableReason = *reason;
            ++changes.unavailable;
            record(row, &stored);
            continue;
        }
        ++changes.removed;
        changeSet.removed.push_back(ChangedGame{stored.id, plan.name, stored.entry.key});
        if (!m_options.rebuildIndex) {
            m_writes.removals.push_back(stored.id);
        }
    }
    return ok();
}

void ImportRun::buildGroupWrites(SystemPlan& plan) {
    std::unordered_map<std::string_view, std::size_t> byPath;
    byPath.reserve(plan.entries.size());
    for (std::size_t i = 0; i < plan.entries.size(); ++i) {
        byPath.emplace(m_snapshot.entries[plan.entries[i]].key.relativePath(), i);
    }
    std::vector<IndexedDiscGroup> groups;
    for (const DiscGroup& group : m_snapshot.discGroups) {
        if (group.system != plan.name) {
            continue;
        }
        const auto head = byPath.find(group.head.relativePath());
        if (head == byPath.end()) {
            continue; // a head is an entry of its system: never the case
        }
        IndexedDiscGroup indexed;
        indexed.head = plan.entryIds[head->second];
        indexed.cached.group = group;
        if (const auto& file = m_snapshot.entries[plan.entries[head->second]].file) {
            indexed.cached.headSize = file->size;
            indexed.cached.headLastWriteTime = file->lastWriteTime;
        }
        bool complete = true;
        for (const LookupKey& member : group.members) {
            const auto found = byPath.find(member.relativePath());
            if (found == byPath.end()) {
                complete = false;
                break;
            }
            indexed.members.push_back(plan.entryIds[found->second]);
        }
        if (complete) {
            groups.push_back(std::move(indexed));
        }
    }
    // The groups the index held whose head is under a folder that cannot be listed stay, as their rows do.
    for (const IndexedDiscGroup& stored : plan.groups) {
        const bool replaced = std::any_of(groups.begin(), groups.end(),
                                          [&](const IndexedDiscGroup& group) { return group.head == stored.head; });
        if (!replaced && plan.unavailable.reasonFor(stored.cached.group.head.key())) {
            groups.push_back(stored);
        }
    }
    for (const IndexedDiscGroup& group : groups) {
        const auto stored = std::find_if(plan.groups.begin(), plan.groups.end(),
                                         [&](const IndexedDiscGroup& candidate) { return candidate.head == group.head; });
        if (m_options.rebuildIndex || stored == plan.groups.end() || !(*stored == group)) {
            m_writes.groups.push_back(group);
        }
    }
    if (!m_options.rebuildIndex) {
        for (const IndexedDiscGroup& stored : plan.groups) {
            if (std::none_of(groups.begin(), groups.end(),
                             [&](const IndexedDiscGroup& group) { return group.head == stored.head; })) {
                m_writes.groupRemovals.push_back(stored.head);
            }
        }
    }
}

Status ImportRun::buildWrites(SystemPlan& plan) {
    const auto putSystem = [&](const SystemInfo& info) {
        if (m_options.rebuildIndex || !plan.storedInfo || !(*plan.storedInfo == info)) {
            m_writes.systems.push_back(info);
        }
    };
    if (!plan.apply) {
        // Kept as it was: in a rebuild, its rows, groups and system row are written again as the index held them. A
        // gamelist that failed still updates the system row (its state "failed", the alternative emulator it held).
        const bool gamelistFailed = plan.changes.outcome == SystemOutcome::GamelistFailed ||
                                    plan.changes.outcome == SystemOutcome::MetadataPending;
        if (gamelistFailed && plan.info != nullptr) {
            SystemInfo info = *plan.info;
            info.alternativeEmulator = plan.storedInfo ? plan.storedInfo->alternativeEmulator : std::string();
            putSystem(info);
        } else if (m_options.rebuildIndex && plan.storedInfo) {
            m_writes.systems.push_back(*plan.storedInfo);
        }
        if (m_options.rebuildIndex) {
            m_writes.rows.insert(m_writes.rows.end(), plan.rows.begin(), plan.rows.end());
            m_writes.groups.insert(m_writes.groups.end(), plan.groups.begin(), plan.groups.end());
        }
        return ok();
    }
    if (plan.info == nullptr) {
        // No longer configured (and accepted): its rows and system row go, its identities stay, not present.
        buildIdentityWrites(plan);
        for (const IndexedEntry& row : plan.rows) {
            ++plan.changes.removed;
            m_result.changes.removed.push_back(ChangedGame{row.id, plan.name, row.entry.key});
            if (!m_options.rebuildIndex) {
                m_writes.removals.push_back(row.id);
            }
        }
        if (plan.storedInfo && !m_options.rebuildIndex) {
            m_writes.removedSystems.push_back(plan.name);
        }
        return ok();
    }
    putSystem(*plan.info);
    buildIdentityWrites(plan);
    if (auto rows = buildRowWrites(plan); !rows) {
        return rows;
    }
    buildGroupWrites(plan);
    return ok();
}

Status ImportRun::write() {
    IImportTransaction& transaction = *m_transaction;
    ImportStatistics& statistics = m_result.statistics;
    std::size_t sinceCheck = 0;
    const auto step = [&]() -> Status {
        if (++sinceCheck >= kWritesBetweenCancelChecks) {
            sinceCheck = 0;
            if (cancelled()) {
                return cancelledError();
            }
        }
        return ok();
    };
    const auto byId = [](const auto& a, const auto& b) { return a.id < b.id; };
    std::sort(m_writes.identitiesBefore.begin(), m_writes.identitiesBefore.end(), byId);
    std::sort(m_writes.identitiesNew.begin(), m_writes.identitiesNew.end(), byId);
    std::sort(m_writes.rows.begin(), m_writes.rows.end(), byId);
    std::sort(m_writes.removals.begin(), m_writes.removals.end());
    std::sort(m_writes.groups.begin(), m_writes.groups.end(),
              [](const IndexedDiscGroup& a, const IndexedDiscGroup& b) { return a.head < b.head; });
    std::sort(m_writes.groupRemovals.begin(), m_writes.groupRemovals.end());

    if (auto put = transaction.putLibrary(m_profile); !put) {
        return put;
    }
    auto suspended = transaction.prepareWrites(m_writes.rows.size() + m_writes.removals.size());
    if (!suspended) {
        return propagate(suspended.error());
    }
    statistics.searchIndexRebuilt = *suspended;
    if (m_options.rebuildIndex) {
        if (auto cleared = transaction.clearIndex(m_profile.id); !cleared) {
            return cleared;
        }
    }
    for (const SystemInfo& system : m_writes.systems) {
        if (auto put = transaction.putSystem(m_profile.id, system); !put) {
            return put;
        }
        ++statistics.systemsWritten;
    }
    for (const auto* identities : {&m_writes.identitiesBefore, &m_writes.identitiesNew}) {
        for (const StoredIdentity& identity : *identities) {
            if (auto put = transaction.putIdentity(identity); !put) {
                return put;
            }
            ++statistics.identitiesWritten;
            if (auto checked = step(); !checked) {
                return checked;
            }
        }
    }
    for (const IndexedEntry& row : m_writes.rows) {
        if (auto put = transaction.putEntry(row); !put) {
            return put;
        }
        ++statistics.entriesWritten;
        if (auto checked = step(); !checked) {
            return checked;
        }
    }
    for (const GameId id : m_writes.removals) {
        if (auto removed = transaction.removeEntry(id); !removed) {
            return removed;
        }
        ++statistics.entriesRemoved;
        if (auto checked = step(); !checked) {
            return checked;
        }
    }
    for (const SystemName& system : m_writes.removedSystems) {
        if (auto removed = transaction.removeSystem(m_profile.id, system); !removed) {
            return removed;
        }
    }
    for (const GameId head : m_writes.groupRemovals) {
        if (auto removed = transaction.removeDiscGroup(head); !removed) {
            return removed;
        }
        ++statistics.discGroupsRemoved;
    }
    for (const IndexedDiscGroup& group : m_writes.groups) {
        if (auto put = transaction.putDiscGroup(group); !put) {
            return put;
        }
        ++statistics.discGroupsWritten;
    }
    if (auto issues = transaction.putImportIssues(m_profile.id, m_result.changes.issues, m_options.systems); !issues) {
        return issues;
    }
    if (cancelled()) {
        return cancelledError();
    }
    ImportRecord record;
    record.library = m_profile.id;
    record.kind = m_options.rebuildIndex ? ImportKind::RebuildIndex : ImportKind::Import;
    record.startedAt = m_startedAt;
    record.finishedAt = core::utcNow();
    record.acceptRemovals = m_options.acceptRemovals;
    record.systemsAsked = m_options.systems;
    record.systems = m_result.changes.systems;
    if (auto recorded = transaction.recordImport(record); !recorded) {
        return recorded;
    }
    return transaction.commit();
}

Expected<ImportResult> ImportRun::run() {
    const Clock::time_point start = Clock::now();
    m_startedAt = core::utcNow();
    m_keyVersion = currentLookupKeyVersion();
    ImportStatistics& statistics = m_result.statistics;
    ChangeSet& changes = m_result.changes;

    // 1. The library: the store is the authority on its id. 2. One import at a time, from the start.
    if (m_options.rebuildIndex && !m_options.systems.empty()) {
        return makeError(ErrorCode::InvalidArgument, "the index is rebuilt for the whole library, never for some systems");
    }
    if (auto checked = checkLibrary(); !checked) {
        return propagate(checked.error());
    }
    auto lock = m_store.lockImport(m_profile.id);
    if (!lock) {
        return propagate(lock.error());
    }
    m_lock = std::move(*lock);

    // 3. The snapshot, with the disc groups the index holds as its cache, then the media of its entries.
    auto groups = m_store.discGroups(m_profile.id);
    if (!groups) {
        return propagate(groups.error());
    }
    m_storedGroups = std::move(*groups);
    std::vector<CachedDiscGroup> cache;
    cache.reserve(m_storedGroups.size());
    for (const IndexedDiscGroup& group : m_storedGroups) {
        cache.push_back(group.cached);
    }
    LibraryLoadOptions load;
    load.cancel = m_options.cancel;
    load.discGroups = cache;
    auto snapshot = m_source.load(m_profile, load);
    if (!snapshot) {
        return propagate(snapshot.error());
    }
    m_snapshot = std::move(*snapshot);
    statistics.entries = m_snapshot.entries.size();
    statistics.loadTime = Clock::now() - start;
    for (const SystemName& name : m_options.systems) {
        if (m_snapshot.findSystem(name) == nullptr) {
            return makeError(ErrorCode::InvalidArgument, "the library gives no system of that name", name.str());
        }
    }
    if (cancelled()) {
        return cancelledError();
    }
    const Clock::time_point mediaStart = Clock::now();
    if (auto asked = askMedia(); !asked) {
        return propagate(asked.error());
    }
    statistics.mediaTime = Clock::now() - mediaStart;

    // 4. The write transaction, then what the store holds.
    const Clock::time_point compareStart = Clock::now();
    auto transaction = m_store.beginImport(m_profile.id);
    if (!transaction) {
        return propagate(transaction.error());
    }
    m_transaction = std::move(*transaction);
    auto taken = m_store.takenGameIds();
    auto storedSystems = m_store.systems(m_profile.id);
    auto identitySystems = m_store.identitySystems(m_profile.id);
    if (!taken || !storedSystems || !identitySystems) {
        return propagate(!taken ? taken.error() : !storedSystems ? storedSystems.error() : identitySystems.error());
    }
    m_taken = std::move(*taken);
    changes.firstImport = storedSystems->empty() && identitySystems->empty();

    std::vector<SystemName> names;
    for (const SystemInfo& system : m_snapshot.systems) {
        names.push_back(system.name);
    }
    for (const SystemInfo& system : *storedSystems) {
        names.push_back(system.name);
    }
    names.insert(names.end(), identitySystems->begin(), identitySystems->end());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::erase_if(names, [&](const SystemName& name) { return !inScope(name); });

    std::size_t unavailableSystems = 0;
    std::size_t systemsWithFiles = 0;
    for (const SystemName& name : names) {
        if (cancelled()) {
            return cancelledError();
        }
        auto plan = readSystem(name, *storedSystems);
        if (!plan) {
            return propagate(plan.error());
        }
        SystemPlan& system = *plan;
        const SystemInfo* info = system.info;
        const bool hadFiles = std::any_of(system.identities.begin(), system.identities.end(),
                                          [](const StoredIdentity& identity) { return identity.present; });
        systemsWithFiles += hadFiles ? 1 : 0;
        if (info != nullptr && info->gamelist == GamelistState::Failed) {
            // ADR-0008 §7: a gamelist that failed keeps the system's rows; on its first import, metadata pending.
            if (system.rows.empty()) {
                keepAsItWas(system, SystemOutcome::MetadataPending);
                addIssue(IssueSeverity::Warning, kImportIssueGamelistKept, name,
                         "its gamelist cannot be read: nothing is imported for it until it can (metadata pending)");
            } else {
                keepAsItWas(system, SystemOutcome::GamelistFailed);
                addIssue(IssueSeverity::Warning, kImportIssueGamelistKept, name,
                         fmt::format("its gamelist cannot be read: its {} row(s) are kept as they were",
                                     system.rows.size()));
            }
            m_plans.push_back(std::move(system));
            continue;
        }
        if (info != nullptr && info->availability != Availability::Available) {
            system.changes.outcome = SystemOutcome::SourceUnavailable;
            unavailableSystems += hadFiles ? 1 : 0;
        }
        if (info == nullptr) {
            system.changes.outcome = SystemOutcome::NotConfigured;
        }
        if (auto matched = match(system); !matched) {
            return propagate(matched.error());
        }
        // The mass-disappearance guard (ADR-0008 §4), on what stays gone once the pairing pass has run.
        const SystemChanges& figures = system.changes;
        if (!m_options.acceptRemovals && figures.presentBefore > 0 && figures.gone * 2 > figures.presentBefore) {
            const std::size_t gone = figures.gone;
            const std::size_t present = figures.presentBefore;
            keepAsItWas(system, SystemOutcome::KeptByGuard);
            system.changes.gone = gone;
            system.changes.presentBefore = present;
            addIssue(IssueSeverity::Warning, kImportIssueMassDisappearance, name,
                     info == nullptr
                         ? fmt::format("the configuration no longer defines it while {} of its file(s) were present: "
                                       "nothing changed (accept the removals to remove its rows)",
                                       present)
                         : fmt::format("{} of its {} present file(s) are gone: nothing changed (accept the removals "
                                       "to apply them)",
                                       gone, present));
        } else if (info == nullptr) {
            addIssue(IssueSeverity::Warning, kImportIssueSystemNotConfigured, name,
                     fmt::format("the configuration no longer defines it: its {} row(s) are removed",
                                 system.rows.size()));
        }
        m_plans.push_back(std::move(system));
    }
    if (unavailableSystems > 0 && unavailableSystems == systemsWithFiles) {
        addIssue(IssueSeverity::Warning, kImportIssueRomRootUnavailable, SystemName{},
                 fmt::format("every system folder that held files ({}) is missing or cannot be listed: the ROM root "
                             "may be unplugged, renamed or empty; their rows are kept as source unavailable",
                             unavailableSystems));
    }

    if (auto assigned = assignIds(); !assigned) {
        return propagate(assigned.error());
    }
    for (SystemPlan& plan : m_plans) {
        if (auto built = buildWrites(plan); !built) {
            return propagate(built.error());
        }
    }

    // The ChangeSet: the snapshot's issues, then the import's; per system, the issues naming it.
    changes.issues = m_snapshot.issues;
    changes.issues.insert(changes.issues.end(), m_issues.begin(), m_issues.end());
    for (SystemPlan& plan : m_plans) {
        plan.changes.issues = static_cast<std::size_t>(
            std::count_if(changes.issues.begin(), changes.issues.end(),
                          [&](const Issue& issue) { return issue.system == plan.name.str(); }));
        changes.systems.push_back(plan.changes);
    }
    statistics.compareTime = Clock::now() - compareStart;
    if (cancelled()) {
        return cancelledError();
    }

    const Clock::time_point writeStart = Clock::now();
    if (auto written = write(); !written) {
        return propagate(written.error());
    }
    statistics.writeTime = Clock::now() - writeStart;
    statistics.totalTime = Clock::now() - start;
    const SystemChanges total = changes.total();
    spdlog::info("Import of library '{}' committed: {} added, {} changed, {} removed, {} unchanged, {} unavailable, "
                 "{} kept; {} new identities; {} identity and {} entry rows written, {} removed in {:.2f} s",
                 m_profile.id.str(), total.added, total.changed, total.removed, total.unchanged, total.unavailable,
                 total.kept, total.newIdentities, statistics.identitiesWritten, statistics.entriesWritten,
                 statistics.entriesRemoved, std::chrono::duration<double>(statistics.totalTime).count());
    return std::move(m_result);
}

} // namespace

Expected<ImportResult> ImportService::run(const LibraryProfile& profile, const ImportOptions& options) {
    ImportRun run(m_source, m_media, m_store, m_pairing, profile, options);
    return run.run();
}

} // namespace rv::catalog
