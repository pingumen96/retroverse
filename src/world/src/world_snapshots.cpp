#include <rv/world/world_snapshots.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <system_error>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/plan_json.hpp>

#include "data_reader.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;
using detail::TransactionStop;

/// A snapshot being written: "<name>.partial", renamed to "<name>" once whole.
constexpr std::string_view kPartialSuffix = ".partial";
/// The names tried for one millisecond and action: "<time>-<action>", then "-2" ...
constexpr unsigned kMaxNameCounter = 1000;
/// snapshot.json and the journal are a few hundred bytes.
constexpr std::uintmax_t kMaxRecordBytes = 64 * 1024;

std::string lossy(const std::filesystem::path& path) {
    return core::pathToUtf8Lossy(path);
}

/// The files a snapshot keeps, in the order a restore writes them (ADR-0015 §6: the overrides, then the plan; the
/// meta sidecar, informative, with its plan).
struct KeptFile {
    std::filesystem::path WorldFiles::* world;
    std::string_view name;
    std::string WorldSnapshot::* digest;
};
const std::array<KeptFile, 3> kKeptFiles{{
    {&WorldFiles::overrides, kOverridesFileName, &WorldSnapshot::overridesDigest},
    {&WorldFiles::meta, kPlanMetaFileName, &WorldSnapshot::metaDigest},
    {&WorldFiles::plan, kPlanFileName, &WorldSnapshot::planDigest},
}};

std::span<const std::byte> bytesOf(std::string_view text) {
    return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

/// "20261027T101112.123Z": formatUtcIso8601() without the separators a folder name cannot hold (':'), as the plans'
/// backups name their time.
std::string folderTime(std::chrono::system_clock::time_point time) {
    std::string text;
    for (const char c : core::formatUtcIso8601(time)) {
        if (c != '-' && c != ':') {
            text += c;
        }
    }
    return text;
}

/// A snapshot's folder name as this program makes them: letters, digits, '.', '-'; never "." or "..".
bool isSnapshotName(std::string_view name) {
    if (name.empty() || name.size() > 128 || name == "." || name == ".." || name.ends_with(kPartialSuffix)) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
    });
}

std::filesystem::path folderOf(const WorldFiles& files, std::string_view name) {
    return files.snapshots / core::pathFromUtf8(name);
}

/// A file's bytes, or nothing when it is not there.
Expected<std::optional<std::vector<std::byte>>> readIfThere(const std::filesystem::path& path) {
    auto bytes = core::readBinaryFile(path);
    if (!bytes) {
        if (bytes.error().code == ErrorCode::NotFound) {
            return std::optional<std::vector<std::byte>>{};
        }
        return propagate(bytes.error());
    }
    return std::optional<std::vector<std::byte>>(std::move(*bytes));
}

Expected<WorldSnapshot> readRecordIn(const std::filesystem::path& folder) {
    const std::filesystem::path file = folder / std::filesystem::path(kSnapshotFileName);
    auto text = core::readTextFile(file, kMaxRecordBytes);
    if (!text) {
        return propagate(text.error());
    }
    auto record = readSnapshotRecord(*text, lossy(folder.filename()) + "/" + std::string(kSnapshotFileName));
    if (!record) {
        return propagate(record.error());
    }
    record->name = lossy(folder.filename());
    return record;
}

Status writeRecordIn(const std::filesystem::path& folder, const WorldSnapshot& snapshot) {
    return core::writeTextFileAtomic(folder / std::filesystem::path(kSnapshotFileName), writeSnapshotRecord(snapshot));
}

/// Removes a folder and what it holds; false (logged) when it could not be.
bool removeFolder(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (error) {
        spdlog::warn("World store: a snapshot folder could not be removed: {} ({})", lossy(folder.filename()),
                     error.message());
        return false;
    }
    return true;
}

/// Copies the world's files into a new snapshot (a "<name>.partial" folder renamed whole), with its record.
Expected<WorldSnapshot> takeSnapshot(const WorldFiles& files, WorldSnapshot snapshot,
                                     std::chrono::system_clock::time_point now,
                                     const std::vector<WorldSnapshot>& existing) {
    for (const WorldSnapshot& kept : existing) {
        snapshot.sequence = std::max(snapshot.sequence, kept.sequence);
    }
    snapshot.sequence += 1;
    snapshot.takenAt = core::formatUtcIso8601(now);
    const std::string stem = folderTime(now) + "-" + snapshot.action;
    std::error_code error;
    for (unsigned counter = 1; counter <= kMaxNameCounter && snapshot.name.empty(); ++counter) {
        const std::string name = counter == 1 ? stem : stem + "-" + std::to_string(counter);
        const bool taken = std::filesystem::exists(folderOf(files, name), error) ||
                           std::filesystem::exists(folderOf(files, name + std::string(kPartialSuffix)), error);
        if (error) {
            return makeError(ErrorCode::IoError, "cannot look into the snapshots' folder: " + error.message(),
                             lossy(files.snapshots));
        }
        if (!taken) {
            snapshot.name = name;
        }
    }
    if (snapshot.name.empty()) {
        return makeError(ErrorCode::Conflict, "no free snapshot name for this time", lossy(files.snapshots));
    }
    const std::filesystem::path partial = folderOf(files, snapshot.name + std::string(kPartialSuffix));
    std::filesystem::create_directories(partial, error);
    if (error) {
        return makeError(ErrorCode::IoError, "cannot create a snapshot's folder: " + error.message(), lossy(partial));
    }
    const auto fail = [&](Error why) {
        removeFolder(partial);
        return propagate(std::move(why));
    };
    for (const KeptFile& kept : kKeptFiles) {
        auto bytes = readIfThere(files.*kept.world);
        if (!bytes) {
            return fail(bytes.error());
        }
        snapshot.*kept.digest = "";
        if (*bytes) {
            if (Status written = core::writeFileAtomic(partial / std::filesystem::path(kept.name), **bytes);
                !written) {
                return fail(written.error());
            }
            snapshot.*kept.digest = core::sha256Hex(**bytes);
        }
    }
    if (Status written = writeRecordIn(partial, snapshot); !written) {
        return fail(written.error());
    }
    std::filesystem::rename(partial, folderOf(files, snapshot.name), error);
    if (error) {
        return fail(Error{ErrorCode::IoError, "cannot complete a snapshot: " + error.message(), lossy(partial)});
    }
    return snapshot;
}

/// The bytes of a snapshot's files, checked against its digests (nothing when the file was not there), in kKeptFiles'
/// order.
Expected<std::vector<std::optional<std::vector<std::byte>>>> readKeptFiles(const WorldFiles& files,
                                                                           const WorldSnapshot& snapshot) {
    const std::filesystem::path folder = folderOf(files, snapshot.name);
    std::vector<std::optional<std::vector<std::byte>>> kept;
    for (const KeptFile& file : kKeptFiles) {
        const std::string& digest = snapshot.*file.digest;
        if (digest.empty()) {
            kept.emplace_back();
            continue;
        }
        const std::filesystem::path path = folder / std::filesystem::path(file.name);
        auto bytes = core::readBinaryFile(path);
        if (!bytes) {
            return propagate(bytes.error());
        }
        if (core::sha256Hex(*bytes) != digest) {
            return makeError(ErrorCode::ValidationFailed,
                             "the snapshot is damaged: " + std::string(file.name) + " is not the file it kept",
                             lossy(path));
        }
        kept.emplace_back(std::move(*bytes));
    }
    return kept;
}

/// Writes the first `count` of a snapshot's files (kKeptFiles' order) over the world's, byte for byte; a file the
/// snapshot did not hold is removed.
Status restoreFiles(const WorldFiles& files, const std::vector<std::optional<std::vector<std::byte>>>& kept,
                    std::size_t count) {
    for (std::size_t i = 0; i < kKeptFiles.size() && i < count; ++i) {
        const std::filesystem::path& path = files.*kKeptFiles[i].world;
        if (kept[i]) {
            if (Status written = core::writeFileAtomic(path, *kept[i]); !written) {
                return written;
            }
            continue;
        }
        std::error_code error;
        std::filesystem::remove(path, error);
        if (error) {
            return makeError(ErrorCode::IoError, "cannot remove a file the snapshot did not hold: " + error.message(),
                             lossy(path));
        }
    }
    return ok();
}

std::string writeJournal(const WorldSnapshot& snapshot) {
    const nlohmann::json document{{"schemaVersion", kTransactionSchemaVersion},
                                  {"snapshot", snapshot.name},
                                  {"action", snapshot.action}};
    return document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

/// The snapshot a journal names.
Expected<std::string> readJournal(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    const ObjectReader reader(document, *value, "", {"action", "schemaVersion", "snapshot"});
    (void)reader.integer("schemaVersion", kTransactionSchemaVersion, kTransactionSchemaVersion);
    (void)reader.checkedString("action", isValidActionName, "an action name");
    std::string snapshot = reader.checkedString("snapshot", isSnapshotName, "a snapshot's folder name");
    if (document.failed()) {
        return propagate(document.error());
    }
    return snapshot;
}

Status removeJournal(const WorldFiles& files) {
    std::error_code error;
    std::filesystem::remove(files.transaction, error);
    if (error) {
        return makeError(ErrorCode::IoError, "cannot remove the journal: " + error.message(), lossy(files.transaction));
    }
    return ok();
}

/// Removes the snapshots beyond the newest kSnapshotsKept, returning their names.
std::vector<std::string> rotateSnapshots(const WorldFiles& files) {
    std::vector<std::string> removed;
    const std::vector<WorldSnapshot> snapshots = listSnapshots(files);
    for (std::size_t i = kSnapshotsKept; i < snapshots.size(); ++i) {
        if (removeFolder(folderOf(files, snapshots[i].name))) {
            removed.push_back(snapshots[i].name);
        }
    }
    return removed;
}

/// Rolls back right away the action whose journal was just written (a write failed): the next lock would do the same.
template <class T>
Expected<T> rollBackAfter(const WorldFiles& files, Error why) {
    if (auto restored = detail::recoverInterruptedAction(files); !restored) {
        spdlog::error("World store: an action failed ({}) and its rollback too ({}); the next lock tries again",
                      why.toString(), restored.error().toString());
    }
    return propagate(std::move(why));
}

Error stopped() {
    return Error{ErrorCode::Cancelled, "stopped where a test asked", "world snapshots"};
}

/// "edit of room:snes", "regenerate" (no scope): for the log.
std::string actionText(std::string_view action, std::string_view scope) {
    return scope.empty() ? std::string(action) : std::string(action) + " of " + std::string(scope);
}

std::chrono::system_clock::time_point timeOf(const std::function<std::chrono::system_clock::time_point()>& clock) {
    return clock ? clock() : std::chrono::system_clock::now();
}

} // namespace

bool isValidActionName(std::string_view text) noexcept {
    if (text.empty() || text.size() > 32 || !(text.front() >= 'a' && text.front() <= 'z')) {
        return false;
    }
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

std::string writeSnapshotRecord(const WorldSnapshot& snapshot) {
    const nlohmann::json document{{"schemaVersion", snapshot.schemaVersion},
                                  {"sequence", snapshot.sequence},
                                  {"kind", std::string(toString(snapshot.kind))},
                                  {"action", snapshot.action},
                                  {"scope", snapshot.scope},
                                  {"writer", snapshot.writer},
                                  {"takenAt", snapshot.takenAt},
                                  {"planDigest", snapshot.planDigest},
                                  {"metaDigest", snapshot.metaDigest},
                                  {"overridesDigest", snapshot.overridesDigest},
                                  {"undone", snapshot.undone},
                                  {"undoes", snapshot.undoes}};
    return document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

Expected<WorldSnapshot> readSnapshotRecord(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    if (value->is_object()) {
        const auto found = value->find("schemaVersion");
        if (found != value->end() && found->is_number_integer() &&
            found->get<std::int64_t>() > kSnapshotSchemaVersion) {
            document.fail("/schemaVersion", "written by a newer Retroverse", ErrorCode::Unsupported);
            return propagate(document.error());
        }
    }
    const ObjectReader reader(document, *value, "",
                              {"action", "kind", "metaDigest", "overridesDigest", "planDigest", "schemaVersion",
                               "scope", "sequence", "takenAt", "undoes", "undone", "writer"});
    const auto digest = [&](std::string_view key) {
        return reader.checkedString(
            key, [](std::string_view text) { return text.empty() || isSha256Hex(text); },
            "empty or a SHA-256 in lowercase hexadecimal");
    };
    WorldSnapshot snapshot;
    snapshot.schemaVersion = reader.integer("schemaVersion", kSnapshotSchemaVersion, kSnapshotSchemaVersion);
    snapshot.sequence =
        static_cast<std::uint64_t>(reader.integer("sequence", 1, std::numeric_limits<std::int64_t>::max()));
    snapshot.kind = reader.enumeration<SnapshotKind>("kind", parseSnapshotKind, "one of action, undo");
    snapshot.action = reader.checkedString("action", isValidActionName, "an action name");
    snapshot.scope = reader.string("scope");
    snapshot.writer = reader.string("writer");
    snapshot.takenAt = reader.checkedString(
        "takenAt", [](std::string_view text) { return core::parseUtcIso8601(text).has_value(); }, "a UTC time");
    snapshot.planDigest = digest("planDigest");
    snapshot.metaDigest = digest("metaDigest");
    snapshot.overridesDigest = digest("overridesDigest");
    snapshot.undone = reader.boolean("undone");
    snapshot.undoes = reader.checkedString(
        "undoes", [](std::string_view text) { return text.empty() || isSnapshotName(text); },
        "empty or a snapshot's folder name");
    if (document.failed()) {
        return propagate(document.error());
    }
    return snapshot;
}

std::vector<WorldSnapshot> listSnapshots(const WorldFiles& files) {
    std::vector<WorldSnapshot> snapshots;
    std::error_code error;
    std::filesystem::directory_iterator entries(files.snapshots, error);
    for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        std::error_code kind;
        const std::string name = lossy(entries->path().filename());
        if (!entries->is_directory(kind) || !isSnapshotName(name)) {
            continue; // a half-made snapshot ("<name>.partial") or anything else
        }
        auto record = readRecordIn(entries->path());
        if (!record) {
            spdlog::warn("World store: a snapshot is left out: {}", record.error().toString());
            continue;
        }
        snapshots.push_back(std::move(*record));
    }
    std::sort(snapshots.begin(), snapshots.end(), [](const WorldSnapshot& a, const WorldSnapshot& b) {
        return std::tie(a.sequence, a.name) > std::tie(b.sequence, b.name);
    });
    return snapshots;
}

std::optional<WorldSnapshot> nextUndo(const std::vector<WorldSnapshot>& snapshots) {
    std::optional<WorldSnapshot> next;
    for (const WorldSnapshot& snapshot : snapshots) {
        if (snapshot.kind == SnapshotKind::Action && !snapshot.undone &&
            (!next || snapshot.sequence > next->sequence)) {
            next = snapshot;
        }
    }
    return next;
}

namespace detail {

Expected<CommitReport> commitWorldStoppingAt(const WorldLock& lock, const WorldAction& action,
                                             const GenerationResult& result, const WorldOverrides& overrides,
                                             const SaveOptions& options, TransactionStop stop) {
    const WorldFiles& files = lock.files();
    if (!lock.held()) {
        return makeError(ErrorCode::InvalidArgument, "the world lock is not held (moved from)", "commitWorld");
    }
    if (!isValidActionName(action.action)) {
        return makeError(ErrorCode::InvalidArgument, "not an action name: \"" + action.action + "\"", "commitWorld");
    }
    if (result.changes.outcome == GenerationOutcome::RequiresExplicitRegeneration) {
        return makeError(ErrorCode::InvalidArgument,
                         "the plan needs an explicit regeneration: an update that keeps it has nothing to save",
                         lossy(files.plan));
    }
    if (!result.validation.passed()) {
        return makeError(ErrorCode::ValidationFailed,
                         "the plan breaks a hard rule of its validation and is never saved (ADR-0010 §11)",
                         lossy(files.plan));
    }
    auto planText = writePlan(result.plan);
    if (!planText) {
        return propagate(planText.error());
    }
    auto overridesText = writeOverrides(overrides);
    if (!overridesText) {
        return propagate(overridesText.error());
    }
    // Nothing to do when the folder holds both already (a world without overrides holds no file).
    auto storedPlan = core::readTextFile(files.plan);
    auto storedOverrides = core::readTextFile(files.overrides);
    for (const auto* stored : {&storedPlan, &storedOverrides}) {
        if (!*stored && stored->error().code != ErrorCode::NotFound) {
            return propagate(stored->error());
        }
    }
    const bool planThere = storedPlan && *storedPlan == *planText;
    const bool overridesThere =
        storedOverrides ? *storedOverrides == *overridesText : overrides.empty();
    CommitReport report;
    if (planThere && overridesThere) {
        report.plan.digest = core::sha256Hex(*planText);
        report.overrides.digest = storedOverrides ? core::sha256Hex(*storedOverrides) : std::string();
        return report;
    }

    WorldSnapshot record;
    record.kind = SnapshotKind::Action;
    record.action = action.action;
    record.scope = action.scope;
    record.writer = options.writer;
    auto snapshot = takeSnapshot(files, std::move(record), timeOf(options.clock), listSnapshots(files));
    if (!snapshot) {
        return propagate(snapshot.error());
    }
    if (stop == TransactionStop::AfterSnapshot) {
        return propagate(stopped());
    }
    if (Status journal = core::writeTextFileAtomic(files.transaction, writeJournal(*snapshot)); !journal) {
        removeFolder(folderOf(files, snapshot->name));
        return propagate(journal.error());
    }
    if (stop == TransactionStop::AfterJournal) {
        return propagate(stopped());
    }
    auto savedOverrides = saveOverrides(lock, overrides);
    if (!savedOverrides) {
        return rollBackAfter<CommitReport>(files, savedOverrides.error());
    }
    if (stop == TransactionStop::BetweenWrites) {
        return propagate(stopped());
    }
    auto savedPlan = savePlan(lock, result, options);
    if (!savedPlan) {
        return rollBackAfter<CommitReport>(files, savedPlan.error());
    }
    if (stop == TransactionStop::BeforeJournalEnds) {
        return propagate(stopped());
    }
    // Both written: the action is done once the journal is gone (else the next lock rolls it back, whole).
    if (Status ended = removeJournal(files); !ended) {
        return propagate(ended.error());
    }
    report.written = true;
    report.snapshot = std::move(*snapshot);
    report.plan = std::move(*savedPlan);
    report.overrides = std::move(*savedOverrides);
    report.snapshotsRemoved = rotateSnapshots(files);
    spdlog::info("World store: {} saved behind the snapshot {}", actionText(action.action, action.scope),
                 report.snapshot->name);
    return report;
}

Expected<UndoReport> undoLastActionStoppingAt(const WorldLock& lock, const UndoOptions& options,
                                              TransactionStop stop) {
    const WorldFiles& files = lock.files();
    if (!lock.held()) {
        return makeError(ErrorCode::InvalidArgument, "the world lock is not held (moved from)", "undoLastAction");
    }
    const std::vector<WorldSnapshot> snapshots = listSnapshots(files);
    const std::optional<WorldSnapshot> target = nextUndo(snapshots);
    if (!target) {
        const bool afterUndo = !snapshots.empty() && snapshots.front().kind == SnapshotKind::Undo;
        return makeError(ErrorCode::NotFound,
                         afterUndo ? "nothing to undo: the last change was an undo, which is never undone (no redo), "
                                     "and no earlier action is left"
                                   : "nothing to undo: no explicit action is kept in the world's snapshots",
                         lossy(files.snapshots));
    }
    auto kept = readKeptFiles(files, *target);
    if (!kept) {
        return propagate(kept.error());
    }
    UndoReport report;
    report.restored = *target;
    if (options.dryRun) {
        return report;
    }

    WorldSnapshot record;
    record.kind = SnapshotKind::Undo;
    record.action = "undo";
    record.scope = target->scope;
    record.writer = options.writer;
    record.undoes = target->name;
    auto snapshot = takeSnapshot(files, std::move(record), timeOf(options.clock), snapshots);
    if (!snapshot) {
        return propagate(snapshot.error());
    }
    if (stop == TransactionStop::AfterSnapshot) {
        return propagate(stopped());
    }
    if (Status journal = core::writeTextFileAtomic(files.transaction, writeJournal(*snapshot)); !journal) {
        removeFolder(folderOf(files, snapshot->name));
        return propagate(journal.error());
    }
    if (stop == TransactionStop::AfterJournal) {
        return propagate(stopped());
    }
    if (stop == TransactionStop::BetweenWrites) {
        if (Status restored = restoreFiles(files, *kept, 1); !restored) {
            return rollBackAfter<UndoReport>(files, restored.error());
        }
        return propagate(stopped());
    }
    if (Status restored = restoreFiles(files, *kept, kKeptFiles.size()); !restored) {
        return rollBackAfter<UndoReport>(files, restored.error());
    }
    WorldSnapshot undone = *target;
    undone.undone = true;
    if (Status marked = writeRecordIn(folderOf(files, undone.name), undone); !marked) {
        return rollBackAfter<UndoReport>(files, marked.error());
    }
    if (stop == TransactionStop::BeforeJournalEnds) {
        return propagate(stopped());
    }
    if (Status ended = removeJournal(files); !ended) {
        return propagate(ended.error());
    }
    report.restored = std::move(undone);
    report.kept = std::move(*snapshot);
    report.snapshotsRemoved = rotateSnapshots(files);
    spdlog::info("World store: undid {} (the snapshot {} restored; the state replaced kept as {})",
                 actionText(report.restored.action, report.restored.scope), report.restored.name, report.kept->name);
    return report;
}

Expected<std::optional<std::string>> recoverInterruptedAction(const WorldFiles& files) {
    // Snapshots half made (interrupted before their journal): the action never started.
    std::error_code error;
    std::filesystem::directory_iterator entries(files.snapshots, error);
    std::vector<std::filesystem::path> partial;
    for (; !error && entries != std::filesystem::directory_iterator(); entries.increment(error)) {
        std::error_code kind;
        if (entries->is_directory(kind) && lossy(entries->path().filename()).ends_with(kPartialSuffix)) {
            partial.push_back(entries->path());
        }
    }
    std::sort(partial.begin(), partial.end());
    for (const std::filesystem::path& folder : partial) {
        if (removeFolder(folder)) {
            spdlog::info("World store: removed the snapshot an interrupted action left half made: {}",
                         lossy(folder.filename()));
        }
    }

    auto journal = core::readTextFile(files.transaction, kMaxRecordBytes);
    if (!journal) {
        if (journal.error().code == ErrorCode::NotFound) {
            return std::optional<std::string>{};
        }
        return propagate(journal.error());
    }
    auto name = readJournal(*journal, lossy(files.transaction.filename()));
    if (!name) {
        return propagate(name.error());
    }
    auto snapshot = readRecordIn(folderOf(files, *name));
    if (!snapshot) {
        return propagate(snapshot.error());
    }
    auto kept = readKeptFiles(files, *snapshot);
    if (!kept) {
        return propagate(kept.error());
    }
    if (Status restored = restoreFiles(files, *kept, kKeptFiles.size()); !restored) {
        return propagate(restored.error());
    }
    // An interrupted undo may have marked the snapshot it restored: it is not undone.
    if (snapshot->kind == SnapshotKind::Undo && !snapshot->undoes.empty()) {
        const std::filesystem::path undoneFolder = folderOf(files, snapshot->undoes);
        if (auto undone = readRecordIn(undoneFolder); undone && undone->undone) {
            undone->undone = false;
            if (Status marked = writeRecordIn(undoneFolder, *undone); !marked) {
                return propagate(marked.error());
            }
        }
    }
    if (Status ended = removeJournal(files); !ended) {
        return propagate(ended.error());
    }
    removeFolder(folderOf(files, *name)); // the action never happened: its snapshot is the state as it stands
    spdlog::warn("World store: an interrupted {} was rolled back: the plan and the overrides of the snapshot {} "
                 "restored",
                 snapshot->action, *name);
    return std::optional<std::string>(*name);
}

} // namespace detail

Expected<CommitReport> commitWorld(const WorldLock& lock, const WorldAction& action, const GenerationResult& result,
                                   const WorldOverrides& overrides, const SaveOptions& options) {
    return detail::commitWorldStoppingAt(lock, action, result, overrides, options, TransactionStop::None);
}

Expected<UndoReport> undoLastAction(const WorldLock& lock, const UndoOptions& options) {
    return detail::undoLastActionStoppingAt(lock, options, TransactionStop::None);
}

} // namespace rv::world
