#include <rv/world/world_store.hpp>

#include <algorithm>
#include <limits>
#include <system_error>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <rv/core/backups.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/system_error.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/world_snapshots.hpp>

#include "data_reader.hpp"

// The world lock is a byte range locked with LockFileEx, as the catalog database's process lock is
// (rv_catalog_db's DatabaseLock): the operating system ends it with its process.
#include <windows.h>

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;

/// How often a writer tries the lock again while another holds it.
constexpr std::chrono::milliseconds kLockRetry{10};

std::string lossy(const std::filesystem::path& path) {
    return core::pathToUtf8Lossy(path);
}

/// LockFileEx on the lock file's first byte, exclusive, without waiting: true when taken, false when another holder has
/// it.
Expected<bool> tryLock(HANDLE file, const std::filesystem::path& path) {
    OVERLAPPED overlapped{};
    if (LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) != FALSE) {
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_LOCK_VIOLATION) {
        return false;
    }
    return makeError(core::fileErrorCode(error, path),
                     "cannot lock the world's lock file: " + core::systemErrorMessage(error), lossy(path));
}

/// A temporary file of core::writeFileAtomic(): "<name>.tmp-<digits>-<digits>".
bool isTemporaryFileName(std::wstring_view name) {
    const std::size_t at = name.rfind(L".tmp-");
    if (at == std::wstring_view::npos || at == 0) {
        return false;
    }
    const std::wstring_view rest = name.substr(at + 5);
    const std::size_t dash = rest.find(L'-');
    if (dash == std::wstring_view::npos || dash == 0 || dash + 1 == rest.size()) {
        return false;
    }
    const auto digits = [](std::wstring_view text) {
        return std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    };
    return digits(rest.substr(0, dash)) && digits(rest.substr(dash + 1));
}

/// Removes the temporary files of interrupted writes in `folder` (not below it), returning them.
std::vector<std::filesystem::path> removeTemporaryFiles(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> removed;
    std::error_code error;
    std::filesystem::directory_iterator entries(folder, error);
    if (error) {
        return removed; // a folder not there yet has none
    }
    std::vector<std::filesystem::path> found;
    for (const std::filesystem::directory_entry& entry : entries) {
        std::error_code kind;
        if (entry.is_regular_file(kind) && isTemporaryFileName(entry.path().filename().native())) {
            found.push_back(entry.path());
        }
    }
    std::sort(found.begin(), found.end());
    for (const std::filesystem::path& path : found) {
        std::error_code removal;
        if (std::filesystem::remove(path, removal)) {
            removed.push_back(path);
        } else if (removal) {
            spdlog::warn("World store: a temporary file of an interrupted write could not be removed: {} ({})",
                         lossy(path.filename()), removal.message());
        }
    }
    return removed;
}

} // namespace

WorldFiles WorldFiles::in(const std::filesystem::path& folder) {
    WorldFiles files;
    files.folder = folder;
    files.plan = folder / std::filesystem::path(kPlanFileName);
    files.meta = folder / std::filesystem::path(kPlanMetaFileName);
    files.overrides = folder / std::filesystem::path(kOverridesFileName);
    files.backups = folder / std::filesystem::path(kPlanBackupsFolderName);
    files.lock = folder / std::filesystem::path(kWorldLockFileName);
    files.snapshots = files.backups / std::filesystem::path(kSnapshotsFolderName);
    files.transaction = folder / std::filesystem::path(kTransactionFileName);
    return files;
}

WorldFiles worldFiles(const std::filesystem::path& userDataRoot, const catalog::LibraryId& library) {
    // A library id is lowercase ASCII letters, digits and '-': a plain folder name.
    return WorldFiles::in(userDataRoot / "worlds" / core::pathFromUtf8(library.str()));
}

Expected<WorldLock> WorldLock::acquire(const WorldFiles& files, std::chrono::milliseconds wait) {
    std::error_code error;
    std::filesystem::create_directories(files.folder, error);
    if (error) {
        return makeError(ErrorCode::IoError, "cannot create the world's folder: " + error.message(),
                         lossy(files.folder));
    }
    // Read and write access (LockFileEx needs one of them), shared for both, never delete sharing: a lock file deleted
    // while held would let the next writer lock a new file apart from this one.
    const HANDLE file =
        CreateFileW(core::extendedLengthPath(files.lock).c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        return makeError(core::fileErrorCode(code, files.lock),
                         "cannot open the world's lock file: " + core::systemErrorMessage(code), lossy(files.lock));
    }
    WorldLock lock(file, files);
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        auto taken = tryLock(file, files.lock);
        if (!taken) {
            return propagate(taken.error());
        }
        if (*taken) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return makeError(ErrorCode::Conflict,
                             "another program writes this world (its lock is held: the app or another "
                             "retroverse-cli); try again when it is done",
                             lossy(files.folder));
        }
        std::this_thread::sleep_for(kLockRetry);
    }
    // A writer that ended mid-write (a crash, a power loss) left its temporary file: the previous file is whole.
    lock.m_removed = removeTemporaryFiles(files.folder);
    for (std::filesystem::path& path : removeTemporaryFiles(files.backups)) {
        lock.m_removed.push_back(std::move(path));
    }
    // A snapshot's record is rewritten in place when an undo marks it (world_snapshots.hpp).
    std::vector<std::filesystem::path> snapshotFolders;
    std::filesystem::directory_iterator snapshots(files.snapshots, error);
    for (; !error && snapshots != std::filesystem::directory_iterator(); snapshots.increment(error)) {
        std::error_code kind;
        if (snapshots->is_directory(kind)) {
            snapshotFolders.push_back(snapshots->path());
        }
    }
    error.clear();
    std::sort(snapshotFolders.begin(), snapshotFolders.end());
    for (const std::filesystem::path& folder : snapshotFolders) {
        for (std::filesystem::path& path : removeTemporaryFiles(folder)) {
            lock.m_removed.push_back(std::move(path));
        }
    }
    if (!lock.m_removed.empty()) {
        spdlog::info("World store: removed {} temporary file(s) of an interrupted write", lock.m_removed.size());
    }
    // An explicit action interrupted between its two writes: its snapshot restored, so that the plan and the overrides
    // agree before anyone writes again.
    auto recovered = detail::recoverInterruptedAction(files);
    if (!recovered) {
        return propagate(recovered.error());
    }
    lock.m_rolledBack = std::move(*recovered);
    return lock;
}

WorldLock::WorldLock(WorldLock&& other) noexcept
    : m_handle(std::exchange(other.m_handle, nullptr)), m_files(std::move(other.m_files)),
      m_removed(std::move(other.m_removed)), m_rolledBack(std::move(other.m_rolledBack)) {}

WorldLock& WorldLock::operator=(WorldLock&& other) noexcept {
    if (this != &other) {
        release();
        m_handle = std::exchange(other.m_handle, nullptr);
        m_files = std::move(other.m_files);
        m_removed = std::move(other.m_removed);
        m_rolledBack = std::move(other.m_rolledBack);
    }
    return *this;
}

WorldLock::~WorldLock() {
    release();
}

void WorldLock::release() noexcept {
    if (m_handle == nullptr) {
        return;
    }
    // Unlocking a byte this handle never locked (a lock that failed to be taken) fails harmlessly.
    OVERLAPPED overlapped{};
    (void)UnlockFileEx(static_cast<HANDLE>(m_handle), 0, 1, 0, &overlapped);
    CloseHandle(static_cast<HANDLE>(m_handle));
    m_handle = nullptr;
}

Expected<StoredPlan> readStoredPlan(const std::filesystem::path& planFile) {
    auto text = core::readTextFile(planFile);
    if (!text) {
        return propagate(text.error());
    }
    auto plan = readPlan(*text, lossy(planFile.filename()));
    if (!plan) {
        return propagate(plan.error());
    }
    return StoredPlan{std::move(*plan), core::sha256Hex(*text)};
}

std::string writePlanMeta(const PlanMeta& meta) {
    const nlohmann::json document{{"schemaVersion", meta.schemaVersion},
                                  {"writtenAt", meta.writtenAt},
                                  {"writer", meta.writer},
                                  {"durationUs", meta.durationUs},
                                  {"planDigest", meta.planDigest},
                                  {"outcome", std::string(toString(meta.outcome))},
                                  {"rebuild", std::string(toString(meta.rebuild))},
                                  {"placements", meta.placements},
                                  {"added", meta.added},
                                  {"kept", meta.kept},
                                  {"unplaced", meta.unplaced},
                                  {"fixturesAdded", meta.fixturesAdded},
                                  {"fixturesRemoved", meta.fixturesRemoved},
                                  {"orphanedPins", meta.orphanedPins},
                                  {"needsConfirmation", meta.needsConfirmation},
                                  {"digestBefore", meta.digestBefore}};
    return document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

Expected<PlanMeta> readPlanMeta(std::string_view text, std::string_view context) {
    auto value = detail::parseDataJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    if (value->is_object()) {
        const auto found = value->find("schemaVersion");
        if (found != value->end() && found->is_number_integer() &&
            found->get<std::int64_t>() > kPlanMetaSchemaVersion) {
            document.fail("/schemaVersion", "written by a newer Retroverse", ErrorCode::Unsupported);
            return propagate(document.error());
        }
    }
    const ObjectReader reader(document, *value, "",
                              {"added", "digestBefore", "durationUs", "fixturesAdded", "fixturesRemoved", "kept",
                               "needsConfirmation", "orphanedPins", "outcome", "placements", "planDigest", "rebuild",
                               "schemaVersion", "unplaced", "writer", "writtenAt"});
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    PlanMeta meta;
    meta.schemaVersion = reader.integer("schemaVersion", kPlanMetaSchemaVersion, kPlanMetaSchemaVersion);
    meta.writtenAt = reader.string("writtenAt");
    meta.writer = reader.string("writer");
    meta.durationUs = reader.integer("durationUs", 0, kMax);
    meta.planDigest = reader.checkedString("planDigest", isSha256Hex, "a SHA-256 in lowercase hexadecimal");
    meta.outcome = reader.enumeration<GenerationOutcome>("outcome", parseGenerationOutcome, "an outcome");
    meta.rebuild = reader.enumeration<RebuildLevel>("rebuild", parseRebuildLevel, "a rebuild level");
    const auto count = [&](std::string_view key) { return static_cast<std::uint64_t>(reader.integer(key, 0, kMax)); };
    meta.placements = count("placements");
    meta.added = count("added");
    meta.kept = count("kept");
    meta.unplaced = count("unplaced");
    meta.fixturesAdded = count("fixturesAdded");
    meta.fixturesRemoved = count("fixturesRemoved");
    meta.orphanedPins = count("orphanedPins");
    meta.needsConfirmation = count("needsConfirmation");
    meta.digestBefore = reader.checkedString(
        "digestBefore", [](std::string_view digest) { return digest.empty() || isSha256Hex(digest); },
        "empty or a SHA-256 in lowercase hexadecimal");
    if (document.failed()) {
        return propagate(document.error());
    }
    return meta;
}

Expected<SaveReport> savePlan(const WorldLock& lock, const GenerationResult& result, const SaveOptions& options) {
    const WorldFiles& files = lock.files();
    if (!lock.held()) {
        return makeError(ErrorCode::InvalidArgument, "the world lock is not held (moved from)", "savePlan");
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
    auto text = writePlan(result.plan);
    if (!text) {
        return propagate(text.error());
    }
    SaveReport report;
    report.digest = core::sha256Hex(*text);
    auto existing = core::readTextFile(files.plan);
    if (existing && *existing == *text) {
        return report; // the folder holds this plan already: no write, no backup
    }
    if (!existing && existing.error().code != ErrorCode::NotFound) {
        return propagate(existing.error());
    }
    if (existing) {
        core::BackupOptions backup;
        backup.keep = kPlanBackupsKept;
        backup.clock = options.clock;
        auto backedUp = core::backUpFile(files.plan, files.backups, backup);
        if (!backedUp) {
            return propagate(backedUp.error());
        }
        report.backup = std::move(backedUp->backup);
        report.backupsRemoved = std::move(backedUp->removed);
    }
    if (Status written = core::writeTextFileAtomic(files.plan, *text); !written) {
        return propagate(written.error());
    }
    report.written = true;

    const ChangeReport& changes = result.changes;
    PlanMeta meta;
    meta.writtenAt = core::formatUtcIso8601(options.clock ? options.clock() : std::chrono::system_clock::now());
    meta.writer = options.writer;
    meta.durationUs = options.duration.count();
    meta.planDigest = report.digest;
    meta.outcome = changes.outcome;
    meta.rebuild = changes.rebuild;
    meta.placements = result.plan.placements.size();
    meta.added = changes.added.size();
    meta.kept = changes.kept;
    meta.unplaced = result.plan.unplaced.size();
    meta.fixturesAdded = changes.fixturesAdded.size();
    meta.fixturesRemoved = changes.fixturesRemoved.size();
    meta.orphanedPins = changes.orphanedPins.size();
    meta.needsConfirmation = changes.needsConfirmation.size();
    meta.digestBefore = changes.digestBefore;
    if (Status written = core::writeTextFileAtomic(files.meta, writePlanMeta(meta)); !written) {
        spdlog::warn("World store: the plan was saved, its meta sidecar not: {}", written.error().toString());
        report.metaError = written.error();
    }
    return report;
}

Expected<OverridesSaveReport> saveOverrides(const WorldLock& lock, const WorldOverrides& overrides) {
    const WorldFiles& files = lock.files();
    if (!lock.held()) {
        return makeError(ErrorCode::InvalidArgument, "the world lock is not held (moved from)", "saveOverrides");
    }
    auto text = writeOverrides(overrides);
    if (!text) {
        return propagate(text.error());
    }
    OverridesSaveReport report;
    auto existing = core::readTextFile(files.overrides);
    if (!existing && existing.error().code != ErrorCode::NotFound) {
        return propagate(existing.error());
    }
    if (!existing && overrides.empty()) {
        return report; // a world without overrides keeps no file
    }
    report.digest = core::sha256Hex(*text);
    if (existing && *existing == *text) {
        return report;
    }
    if (Status written = core::writeTextFileAtomic(files.overrides, *text); !written) {
        return propagate(written.error());
    }
    report.written = true;
    return report;
}

} // namespace rv::world
