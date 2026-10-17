#include <rv/catalog/reconciliation.hpp>

#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <spdlog/spdlog.h>

#include <rv/catalog/identity.hpp>
#include <rv/core/utf8.hpp>

namespace rv::catalog {
namespace {

using Clock = std::chrono::steady_clock;

bool isCancelled(const std::atomic<bool>* cancel) noexcept {
    return cancel != nullptr && cancel->load(std::memory_order_relaxed);
}

QuickHashReader readerOr(QuickHashReader reader) {
    if (reader) {
        return reader;
    }
    return [](const std::filesystem::path& folder, std::string_view relativePath) {
        return readQuickHashBelow(folder, relativePath);
    };
}

/// The absolute path of a file of a system, for issues (the user's own path: never logged above debug level).
std::string filePath(const std::filesystem::path& folder, std::string_view relativePath) {
    std::filesystem::path path = folder;
    if (auto relative = core::tryPathFromUtf8(relativePath)) {
        path /= *relative;
    }
    path.make_preferred();
    return core::pathToUtf8Lossy(path);
}

/// The last element of a key ('/' between the elements).
std::string_view lastElement(std::string_view key) noexcept {
    const std::size_t slash = key.rfind('/');
    return slash == std::string_view::npos ? key : key.substr(slash + 1);
}

/// How many trailing elements two keys share, compared as the file systems compare names (the keys are folded): 0 when
/// the file names differ, 1 for the same file name, more when the folders above it are the same too.
std::size_t commonSuffix(std::string_view a, std::string_view b) noexcept {
    std::size_t shared = 0;
    while (true) {
        const std::string_view lastA = lastElement(a);
        const std::string_view lastB = lastElement(b);
        if (lastA != lastB) {
            return shared;
        }
        ++shared;
        if (lastA.size() == a.size() || lastB.size() == b.size()) {
            return shared; // one key has no element left
        }
        a.remove_suffix(lastA.size() + 1);
        b.remove_suffix(lastB.size() + 1);
    }
}

/// One review row in the making: its candidates (indices into the new files) and why each kind of them is there.
struct ReviewDraft {
    std::set<std::size_t> candidates;
    std::vector<std::string> reasons;

    void add(const std::vector<std::size_t>& files, std::string reason) {
        if (files.empty()) {
            return;
        }
        candidates.insert(files.begin(), files.end());
        reasons.push_back(std::move(reason));
    }
};

std::string plural(std::size_t count, std::string_view one, std::string_view many) {
    return fmt::format("{} {}", count, count == 1 ? one : many);
}

/// "1 new file has", "2 new files have".
std::string newFilesHave(std::size_t count) {
    return plural(count, "new file has", "new files have");
}

} // namespace

std::optional<QuickHash> currentQuickHash(const StoredIdentity& identity) noexcept {
    if (!identity.quickHash || !identity.file || !isCurrent(*identity.quickHash, *identity.file)) {
        return std::nullopt;
    }
    return identity.quickHash;
}

// --- The pairing pass ------------------------------------------------------------------------------------------------

IdentityPairing::IdentityPairing(QuickHashReader reader) : m_reader(readerOr(std::move(reader))) {}

Expected<IdentityPairs> IdentityPairing::pair(const PairingRequest& request) {
    ++m_statistics.requests;
    const std::span<const VanishedIdentity> vanished = request.vanished;
    const std::span<const NewFile> files = request.newFiles;
    IdentityPairs result;

    // The vanished identities' current quick hashes, and the sizes they were taken at: only new files of those sizes
    // are read.
    std::vector<std::optional<QuickHash>> stored(vanished.size());
    std::vector<std::uint64_t> sizes;
    for (std::size_t v = 0; v < vanished.size(); ++v) {
        stored[v] = currentQuickHash(vanished[v].identity);
        if (stored[v]) {
            sizes.push_back(stored[v]->size);
        }
    }
    std::sort(sizes.begin(), sizes.end());
    std::vector<std::optional<QuickHash>> fileHash(files.size());
    std::vector<bool> unreadable(files.size(), false);
    for (std::size_t f = 0; f < files.size(); ++f) {
        const NewFile& file = files[f];
        if (file.file.directory || !std::binary_search(sizes.begin(), sizes.end(), file.file.size)) {
            continue;
        }
        if (isCancelled(request.cancel)) {
            return makeError(ErrorCode::Cancelled, "the pairing pass was cancelled while it read quick hashes");
        }
        auto hash = m_reader(request.system.romFolder, file.key.relativePath());
        if (!hash) {
            unreadable[f] = true;
            ++m_statistics.filesUnreadable;
            spdlog::debug("Reconciliation: cannot read the quick hash of {}: {}",
                          filePath(request.system.romFolder, file.key.relativePath()), hash.error().toString());
            result.issues.push_back(Issue{IssueSeverity::Warning, std::string(kQuickHashIssueUnreadable),
                                          request.system.name.str(),
                                          filePath(request.system.romFolder, file.key.relativePath()),
                                          fmt::format("its quick hash cannot be read ({}): it is not paired by its "
                                                      "hash",
                                                      hash.error().message)});
            continue;
        }
        ++m_statistics.filesHashed;
        m_statistics.bytesRead += quickHashBytes(hash->size);
        fileHash[f] = *hash;
        result.hashes.push_back(NewFileHash{file.key, *hash});
    }

    std::vector<bool> identityUsed(vanished.size(), false);
    std::vector<bool> fileUsed(files.size(), false);
    std::vector<bool> fileMatchesHash(files.size(), false); // its hash is a vanished identity's: rule 1's alone
    std::vector<ReviewDraft> drafts(vanished.size());
    const auto pairWith = [&](std::size_t v, std::size_t f, MoveReason reason) {
        identityUsed[v] = true;
        fileUsed[f] = true;
        result.pairs.push_back(IdentityPair{vanished[v].identity.id, files[f].key, reason});
    };

    // 1. The quick hash, a group per digest and size, in the order of their first identity.
    std::vector<bool> grouped(vanished.size(), false);
    for (std::size_t first = 0; first < vanished.size(); ++first) {
        if (!stored[first] || grouped[first]) {
            continue;
        }
        const auto sameHash = [&](const QuickHash& hash) {
            return hash.digest == stored[first]->digest && hash.size == stored[first]->size;
        };
        std::vector<std::size_t> identities;
        for (std::size_t v = first; v < vanished.size(); ++v) {
            if (stored[v] && sameHash(*stored[v])) {
                identities.push_back(v);
                grouped[v] = true;
            }
        }
        std::vector<std::size_t> group;
        for (std::size_t f = 0; f < files.size(); ++f) {
            if (fileHash[f] && sameHash(*fileHash[f])) {
                group.push_back(f);
                fileMatchesHash[f] = true;
            }
        }
        // Ties: a pair whose identity's best file and whose file's best identity are each other alone, repeatedly.
        const auto score = [&](std::size_t v, std::size_t f) {
            return commonSuffix(vanished[v].identity.key.key(), files[f].key.key());
        };
        while (!identities.empty() && !group.empty()) {
            std::vector<std::pair<std::size_t, std::size_t>> mutual;
            for (const std::size_t v : identities) {
                std::size_t best = 0;
                std::vector<std::size_t> bestFiles;
                for (const std::size_t f : group) {
                    const std::size_t s = score(v, f);
                    if (bestFiles.empty() || s > best) {
                        best = s;
                        bestFiles = {f};
                    } else if (s == best) {
                        bestFiles.push_back(f);
                    }
                }
                if (bestFiles.size() != 1) {
                    continue;
                }
                const std::size_t f = bestFiles.front();
                const bool alone = std::none_of(identities.begin(), identities.end(), [&](std::size_t other) {
                    return other != v && score(other, f) >= best;
                });
                if (alone) {
                    mutual.emplace_back(v, f);
                }
            }
            if (mutual.empty()) {
                break;
            }
            for (const auto& [v, f] : mutual) {
                pairWith(v, f, MoveReason::QuickHash);
            }
            std::erase_if(identities, [&](std::size_t v) { return identityUsed[v]; });
            std::erase_if(group, [&](std::size_t f) { return fileUsed[f]; });
        }
        if (identities.empty() || group.empty()) {
            continue;
        }
        // What ties could not tell apart: identical copies moved together when every score is equal and the counts
        // are; otherwise the user decides.
        const std::size_t level = score(identities.front(), group.front());
        const bool even = std::all_of(identities.begin(), identities.end(), [&](std::size_t v) {
            return std::all_of(group.begin(), group.end(), [&](std::size_t f) { return score(v, f) == level; });
        });
        if (even && identities.size() == group.size()) {
            for (std::size_t k = 0; k < identities.size(); ++k) {
                pairWith(identities[k], group[k], MoveReason::IdenticalCopies);
            }
            continue;
        }
        for (const std::size_t v : identities) {
            drafts[v].add(group, fmt::format("{} its quick hash, and neither name nor path tells which is its file "
                                             "({} vanished identities have it)",
                                             newFilesHave(group.size()), identities.size()));
        }
    }

    // 2. Size, time and name, for the identities without a current hash: a unique file that no other has.
    std::vector<std::vector<std::size_t>> sizeTimeName(vanished.size());
    std::vector<std::size_t> claims(files.size(), 0);
    for (std::size_t v = 0; v < vanished.size(); ++v) {
        const StoredIdentity& identity = vanished[v].identity;
        if (identityUsed[v] || stored[v] || !identity.file) {
            continue;
        }
        const std::string_view name = lastElement(identity.key.key());
        for (std::size_t f = 0; f < files.size(); ++f) {
            const FileFacts& file = files[f].file;
            if (fileUsed[f] || fileMatchesHash[f] || file.directory != identity.file->directory ||
                file.size != identity.file->size || file.lastWriteTime != identity.file->lastWriteTime ||
                lastElement(files[f].key.key()) != name) {
                continue;
            }
            sizeTimeName[v].push_back(f);
            ++claims[f];
        }
    }
    for (std::size_t v = 0; v < vanished.size(); ++v) {
        const std::vector<std::size_t>& candidates = sizeTimeName[v];
        if (candidates.size() == 1 && claims[candidates.front()] == 1) {
            pairWith(v, candidates.front(), MoveReason::SizeTimeName);
        }
    }

    // 3. Review rows for the identities left with candidates.
    for (std::size_t v = 0; v < vanished.size(); ++v) {
        if (identityUsed[v]) {
            continue;
        }
        const StoredIdentity& identity = vanished[v].identity;
        ReviewDraft& draft = drafts[v];
        const auto unpaired = [&](const std::vector<std::size_t>& from) {
            std::vector<std::size_t> left;
            std::copy_if(from.begin(), from.end(), std::back_inserter(left), [&](std::size_t f) { return !fileUsed[f]; });
            return left;
        };
        // Rule 1's ties are in the draft already (the files of a hash group are never paired by rule 2).
        if (stored[v]) {
            std::vector<std::size_t> denied;
            for (std::size_t f = 0; f < files.size(); ++f) {
                if (unreadable[f] && !fileUsed[f] && files[f].file.size == stored[v]->size) {
                    denied.push_back(f);
                }
            }
            draft.add(denied, fmt::format("{} of its size could not be read for its quick hash",
                                          plural(denied.size(), "new file", "new files")));
        } else {
            const std::vector<std::size_t> candidates = unpaired(sizeTimeName[v]);
            if (candidates.size() > 1) {
                draft.add(candidates, fmt::format("{} new files have its size, time and name", candidates.size()));
            } else if (candidates.size() == 1) {
                draft.add(candidates, "the only new file of its size, time and name is another vanished identity's "
                                      "candidate too");
            }
        }
        // The new files of its file name, whatever their content: the user may know them for its file.
        std::vector<std::size_t> named;
        const std::string_view name = lastElement(identity.key.key());
        for (std::size_t f = 0; f < files.size(); ++f) {
            if (!fileUsed[f] && !draft.candidates.contains(f) && lastElement(files[f].key.key()) == name) {
                named.push_back(f);
            }
        }
        draft.add(named, fmt::format("{} its file name but {}", newFilesHave(named.size()),
                                     stored[v] ? "not its quick hash or size" : "not its size or time"));
        if (draft.candidates.empty()) {
            continue; // nothing it could be: deleted, or changed beyond recognition
        }
        ReviewRow review;
        review.game = identity.id;
        review.system = request.system.name;
        review.vanished = identity.key;
        for (const std::size_t f : draft.candidates) { // indices in entry order: key order
            review.candidates.push_back(files[f].key);
        }
        review.reason = fmt::format("{}", fmt::join(draft.reasons, "; "));
        result.reviews.push_back(std::move(review));
    }
    m_statistics.pairs += result.pairs.size();
    m_statistics.reviews += result.reviews.size();
    return result;
}

// --- The quick-hash pass ---------------------------------------------------------------------------------------------

namespace {

/// A file the pass reads.
struct HashCandidate {
    std::size_t system = 0; ///< index into the pass's systems
    GameId id;
    LookupKey key;
    FileFacts file;
};

/// A system of the pass: its ROM folder as the last import recorded it, and whether it can be read.
struct PassSystem {
    SystemName name;
    std::filesystem::path folder;
    bool reachable = true;
};

} // namespace

Expected<QuickHashPassResult> runQuickHashPass(ICatalogStore& store, const LibraryId& library,
                                               const QuickHashPassOptions& options) {
    const Clock::time_point start = Clock::now();
    QuickHashPassResult result;
    const QuickHashReader reader = readerOr(options.reader);

    // One writer of identities at a time: the import reads them before its writes trusting that nothing else does.
    auto lock = store.lockImport(library);
    if (!lock) {
        return propagate(lock.error());
    }
    auto names = store.identitySystems(library);
    auto known = store.systems(library);
    if (!names || !known) {
        return propagate(!names ? names.error() : known.error());
    }
    if (options.system) {
        if (std::find(names->begin(), names->end(), *options.system) == names->end()) {
            return makeError(ErrorCode::InvalidArgument,
                             fmt::format("the library '{}' has no identity of the system", library.str()),
                             options.system->str());
        }
        *names = {*options.system};
    }

    // The candidates: present files without a current quick hash, by system then in the stored order.
    std::vector<PassSystem> systems;
    std::vector<HashCandidate> candidates;
    for (const SystemName& name : *names) {
        auto identities = store.identities(library, name);
        if (!identities) {
            return propagate(identities.error());
        }
        PassSystem system{name, {}, false};
        for (const SystemInfo& info : *known) {
            if (info.name == name) {
                system.folder = info.romFolder;
                system.reachable = info.availability == Availability::Available && !info.romFolder.empty();
            }
        }
        const std::size_t index = systems.size();
        systems.push_back(std::move(system));
        for (const StoredIdentity& identity : *identities) {
            ++result.identities;
            if (!identity.present || !identity.file) {
                ++result.notPresent;
            } else if (identity.file->directory) {
                ++result.folders;
            } else if (currentQuickHash(identity)) {
                ++result.current;
            } else {
                ++result.candidates;
                candidates.push_back(HashCandidate{index, identity.id, identity.key, *identity.file});
            }
        }
    }
    const std::size_t total = options.limit == 0 ? candidates.size() : std::min(candidates.size(), options.limit);

    std::vector<std::pair<GameId, QuickHash>> batch;
    Clock::time_point lastCommit = Clock::now();
    const auto commit = [&]() -> Status {
        if (batch.empty()) {
            return ok();
        }
        auto transaction = store.beginImport(library);
        if (!transaction) {
            return propagate(transaction.error());
        }
        for (const auto& [id, hash] : batch) {
            if (auto put = (*transaction)->putQuickHash(id, hash); !put) {
                return put;
            }
        }
        if (auto committed = (*transaction)->commit(); !committed) {
            return committed;
        }
        result.hashed += batch.size();
        ++result.batches;
        batch.clear();
        lastCommit = Clock::now();
        return ok();
    };
    QuickHashProgress progress;
    progress.total = total;
    std::size_t looked = 0;
    for (const HashCandidate& candidate : candidates) {
        if (looked == total) {
            break;
        }
        if (isCancelled(options.cancel)) {
            result.cancelled = true;
            break;
        }
        ++looked;
        PassSystem& system = systems[candidate.system];
        if (!system.reachable) {
            ++result.unavailable;
        } else if (auto hash = reader(system.folder, candidate.key.relativePath()); !hash) {
            const Error& error = hash.error();
            const std::string file = filePath(system.folder, candidate.key.relativePath());
            spdlog::debug("Quick hash pass: cannot read {}: {}", file, error.toString());
            if (error.code == ErrorCode::NotFound) {
                ++result.gone;
            } else if (error.code == ErrorCode::Unavailable) {
                // The drive or share went: the system's other files are left for a later pass.
                ++result.unavailable;
                system.reachable = false;
                result.issues.push_back(Issue{IssueSeverity::Warning, std::string(kQuickHashIssueUnavailable),
                                              system.name.str(), core::pathToUtf8Lossy(system.folder),
                                              fmt::format("its files cannot be reached ({}): left for a later pass",
                                                          error.message)});
            } else {
                ++result.unreadable;
                result.issues.push_back(Issue{IssueSeverity::Warning, std::string(kQuickHashIssueUnreadable),
                                              system.name.str(), file,
                                              fmt::format("its quick hash cannot be read: {}", error.message)});
            }
        } else if (hash->size != candidate.file.size || hash->lastWriteTime != candidate.file.lastWriteTime) {
            ++result.changed; // not the file the last import saw: hashed once an import has its facts
        } else {
            result.bytesRead += quickHashBytes(hash->size);
            batch.emplace_back(candidate.id, *hash);
            ++progress.hashed;
        }
        progress.done = looked;
        progress.bytesRead = result.bytesRead;
        if (batch.size() >= std::max<std::size_t>(options.batchFiles, 1) ||
            (!batch.empty() && Clock::now() - lastCommit >= options.batchTime)) {
            if (auto committed = commit(); !committed) {
                return propagate(committed.error());
            }
        }
        if (options.progress) {
            options.progress(progress);
        }
    }
    // What was hashed stays, a cancel included.
    if (auto committed = commit(); !committed) {
        return propagate(committed.error());
    }
    result.remaining = result.candidates - looked;
    result.time = Clock::now() - start;
    if (options.progress) {
        options.progress(progress);
    }
    spdlog::info("Quick hash pass of library '{}': {} file(s) hashed, {} changed, {} gone, {} unreadable, {} out of "
                 "reach, {} left, {:.1f} MiB read in {:.2f} s ({} batch(es)){}",
                 library.str(), result.hashed, result.changed, result.gone, result.unreadable, result.unavailable,
                 result.remaining, static_cast<double>(result.bytesRead) / (1024.0 * 1024.0),
                 std::chrono::duration<double>(result.time).count(), result.batches,
                 result.cancelled ? "; cancelled" : "");
    return result;
}

// --- Review resolution -----------------------------------------------------------------------------------------------

namespace {

tl::unexpected<Error> withContext(ErrorCode code, std::string message, std::int64_t review) {
    return makeError(code, std::move(message), fmt::format("review {}", review));
}

/// `group` with the GameId `from` replaced by `to` (head and members); whether it named `from`.
bool repoint(IndexedDiscGroup& group, GameId from, GameId to) {
    bool named = false;
    if (group.head == from) {
        group.head = to;
        named = true;
    }
    for (GameId& member : group.members) {
        if (member == from) {
            member = to;
            named = true;
        }
    }
    return named;
}

} // namespace

Expected<ReviewResolutionResult> resolveReview(ICatalogStore& store, const LibraryId& library,
                                               const ReviewDecision& decision) {
    auto lock = store.lockImport(library);
    if (!lock) {
        return propagate(lock.error());
    }
    auto found = store.review(decision.review);
    if (!found) {
        return propagate(found.error());
    }
    if (!*found) {
        return withContext(ErrorCode::NotFound, "the catalog has no such review row", decision.review);
    }
    const StoredReview stored = std::move(**found);
    auto vanished = store.identity(stored.review.game);
    if (!vanished) {
        return propagate(vanished.error());
    }
    if (!*vanished) {
        return withContext(ErrorCode::NotFound,
                           fmt::format("the review's identity {} is not in the catalog", stored.review.game.toString()),
                           decision.review);
    }
    const StoredIdentity identity = std::move(**vanished);
    if (identity.library != library) {
        return withContext(ErrorCode::InvalidArgument,
                           fmt::format("the review row is of the library '{}'", identity.library.str()),
                           decision.review);
    }
    if (!stored.open()) {
        return withContext(ErrorCode::Conflict,
                           fmt::format("the review row was resolved already ({}, {})",
                                       stored.resolution ? toString(*stored.resolution) : "unknown",
                                       stored.resolvedAt.value_or(std::string())),
                           decision.review);
    }

    ReviewResolutionResult result;
    if (!decision.pairWith) {
        auto transaction = store.beginImport(library);
        if (!transaction) {
            return propagate(transaction.error());
        }
        IdentityEvent event;
        event.game = identity.id;
        event.kind = IdentityEventKind::ReviewResolved;
        event.review = decision.review;
        event.detail = fmt::format("review {} resolved: kept separate from its {}", decision.review,
                                   plural(stored.review.candidates.size(), "candidate", "candidates"));
        if (auto put = (*transaction)->putHistory(event); !put) {
            return propagate(put.error());
        }
        if (auto resolved = (*transaction)->resolveReview(decision.review, ReviewResolution::KeptSeparate, std::nullopt);
            !resolved) {
            return propagate(resolved.error());
        }
        if (auto committed = (*transaction)->commit(); !committed) {
            return propagate(committed.error());
        }
    } else {
        const GameId chosenId = *decision.pairWith;
        auto chosenFound = store.identity(chosenId);
        if (!chosenFound) {
            return propagate(chosenFound.error());
        }
        if (!*chosenFound) {
            auto alias = store.alias(chosenId);
            if (!alias) {
                return propagate(alias.error());
            }
            if (*alias) {
                return withContext(ErrorCode::InvalidArgument,
                                   fmt::format("{} is an alias of {}: pair with that identity", chosenId.toString(),
                                               (*alias)->game.toString()),
                                   decision.review);
            }
            return withContext(ErrorCode::NotFound, fmt::format("the catalog has no identity {}", chosenId.toString()),
                               decision.review);
        }
        const StoredIdentity chosen = std::move(**chosenFound);
        if (chosen.id == identity.id) {
            return withContext(ErrorCode::InvalidArgument, "an identity cannot be paired with itself", decision.review);
        }
        if (chosen.library != identity.library || chosen.system != identity.system) {
            return withContext(ErrorCode::InvalidArgument,
                               fmt::format("{} is of the system '{}', the review's identity of '{}'",
                                           chosen.id.toString(), chosen.system.str(), identity.system.str()),
                               decision.review);
        }
        if (identity.present) {
            return withContext(ErrorCode::Conflict,
                               fmt::format("the file of {} is present again ({}): keep them separate",
                                           identity.id.toString(), identity.key.relativePath()),
                               decision.review);
        }
        if (!chosen.present || !chosen.file) {
            return withContext(ErrorCode::InvalidArgument,
                               fmt::format("the file of {} is not present", chosen.id.toString()), decision.review);
        }

        // What the import would write for this move, read before the transaction (the import lock is held).
        auto rows = store.indexedEntries(library, identity.system);
        auto groups = store.discGroups(library);
        auto taken = store.takenGameIds();
        if (!rows || !groups || !taken) {
            return propagate(!rows ? rows.error() : !groups ? groups.error() : taken.error());
        }
        std::optional<IndexedEntry> chosenRow;
        std::optional<IndexedEntry> oldRow;
        for (const IndexedEntry& row : *rows) {
            if (row.id == chosen.id) {
                chosenRow = row;
            } else if (row.id == identity.id) {
                oldRow = row;
            }
        }
        const LookupKeyVersion keyVersion = currentLookupKeyVersion();
        StoredIdentity moved = identity;
        moved.key = chosen.key;
        moved.keyVersion = keyVersion;
        moved.present = true;
        moved.file = chosen.file;
        moved.quickHash = chosen.quickHash;
        // A row still at the old path (a gamelist entry left there) loses the identity: a new one, as the import gives.
        std::optional<StoredIdentity> left;
        if (oldRow) {
            const GameIdRequest request{identity.system, identity.key};
            auto assigned = assignGameIds(library, std::span<const GameIdRequest>(&request, 1), [&](GameId id) {
                return std::binary_search(taken->begin(), taken->end(), id);
            });
            if (!assigned) {
                return propagate(assigned.error());
            }
            StoredIdentity fresh;
            fresh.id = assigned->front().id;
            fresh.library = library;
            fresh.system = identity.system;
            fresh.key = identity.key;
            fresh.keyVersion = keyVersion;
            left = std::move(fresh);
            result.leftAtOldPath = left->id;
        }
        std::vector<IndexedDiscGroup> touched;
        for (IndexedDiscGroup group : *groups) {
            const bool named = repoint(group, identity.id, left ? left->id : identity.id);
            if (repoint(group, chosen.id, identity.id) || (named && left)) {
                touched.push_back(std::move(group));
            }
        }

        auto transaction = store.beginImport(library);
        if (!transaction) {
            return propagate(transaction.error());
        }
        IImportTransaction& writes = **transaction;
        // The chosen identity becomes the alias first (its state rows move, its index rows go, its path is freed); the
        // vanished identity moves onto its path before the old path's new identity is written.
        if (auto put = writes.putAlias(IdentityAlias{chosen, identity.id, MoveReason::ReviewResolved}); !put) {
            return propagate(put.error());
        }
        if (oldRow) {
            if (auto removed = writes.removeEntry(identity.id); !removed) {
                return propagate(removed.error());
            }
        }
        if (auto put = writes.putIdentity(moved); !put) {
            return propagate(put.error());
        }
        if (left) {
            if (auto put = writes.putIdentity(*left); !put) {
                return propagate(put.error());
            }
        }
        if (chosenRow) {
            IndexedEntry row = *chosenRow;
            row.id = identity.id;
            if (auto put = writes.putEntry(row); !put) {
                return propagate(put.error());
            }
        }
        if (oldRow && left) {
            IndexedEntry row = *oldRow;
            row.id = left->id;
            if (auto put = writes.putEntry(row); !put) {
                return propagate(put.error());
            }
        }
        for (const IndexedDiscGroup& group : touched) {
            if (auto put = writes.putDiscGroup(group); !put) {
                return propagate(put.error());
            }
        }
        MovedGame move{identity.id, identity.system, identity.key, chosen.key, MoveReason::ReviewResolved, chosen.id};
        std::vector<IdentityEvent> events;
        IdentityEvent movedEvent;
        movedEvent.game = identity.id;
        movedEvent.kind = IdentityEventKind::Moved;
        movedEvent.reason = MoveReason::ReviewResolved;
        movedEvent.fromKey = identity.key.key();
        movedEvent.fromPath = identity.key.relativePath();
        movedEvent.toKey = chosen.key.key();
        movedEvent.toPath = chosen.key.relativePath();
        movedEvent.review = decision.review;
        movedEvent.detail = fmt::format("review {} resolved: paired with the file of {}", decision.review,
                                        chosen.id.toString());
        events.push_back(std::move(movedEvent));
        IdentityEvent aliasEvent;
        aliasEvent.game = identity.id;
        aliasEvent.kind = IdentityEventKind::Alias;
        aliasEvent.reason = MoveReason::ReviewResolved;
        aliasEvent.fromKey = chosen.key.key();
        aliasEvent.fromPath = chosen.key.relativePath();
        aliasEvent.alias = chosen.id;
        aliasEvent.review = decision.review;
        aliasEvent.detail = fmt::format("{} stands for this identity from now on (review {})", chosen.id.toString(),
                                        decision.review);
        events.push_back(std::move(aliasEvent));
        IdentityEvent resolvedEvent;
        resolvedEvent.game = identity.id;
        resolvedEvent.kind = IdentityEventKind::ReviewResolved;
        resolvedEvent.review = decision.review;
        resolvedEvent.detail = fmt::format("review {} resolved: paired with {}", decision.review, chosen.id.toString());
        events.push_back(std::move(resolvedEvent));
        for (const IdentityEvent& event : events) {
            if (auto put = writes.putHistory(event); !put) {
                return propagate(put.error());
            }
        }
        if (auto resolved = writes.resolveReview(decision.review, ReviewResolution::Paired, chosen.id); !resolved) {
            return propagate(resolved.error());
        }
        if (auto committed = writes.commit(); !committed) {
            return propagate(committed.error());
        }
        result.moved = std::move(move);
    }
    auto after = store.review(decision.review);
    if (!after) {
        return propagate(after.error());
    }
    if (!*after) {
        return withContext(ErrorCode::NotFound, "the review row went while it was resolved", decision.review);
    }
    result.review = std::move(**after);
    return result;
}

} // namespace rv::catalog
