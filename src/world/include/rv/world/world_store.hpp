#pragma once

// The world's files (ADR-0010 §9; I3 step 9), in <user data>/worlds/<library id>/:
//   world-plan.json       the plan, exactly writePlan()'s canonical text (plan_json.hpp)
//   world-plan.meta.json  what is not canonical: when, by which program and how fast the plan was written, and its
//                         change report's summary (formatted JSON, never hashed, never read to decide anything)
//   backups/              the last kPlanBackupsKept plans before each write, named by UTC time (core::backUpFile)
//   backups/snapshots/    the paired snapshots of the plan and the overrides before each explicit action (I4b step 1,
//                         ADR-0015 §6: world_snapshots.hpp)
//   overrides.json        the pins (overrides.hpp), written by saveOverrides() since I4b step 1
//   transaction.json      the journal of an explicit action in progress (world_snapshots.hpp)
//   world.lock            the world lock: an empty file locked exclusively (LockFileEx) by whoever writes the folder
// The app and retroverse-cli both take the world lock before writing anything there, so that two writers never
// interleave a backup and a write; reading needs no lock (every write is an atomic rename: a reader sees the previous
// file or the new one, whole). The operating system releases the lock of a process that ends, a crash included, and
// the next holder removes the temporary files a crashed write left behind and, when a journal is there, rolls the
// interrupted action back (world_snapshots.hpp: the plan and the overrides never disagree). Callers run the
// data-directory guard (core::checkUserDataDirectory, ADR-0008) before the first write under <user data>;
// retroverse-cli's dispatcher does.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

inline constexpr std::string_view kPlanFileName = "world-plan.json";
inline constexpr std::string_view kPlanMetaFileName = "world-plan.meta.json";
inline constexpr std::string_view kOverridesFileName = "overrides.json";
inline constexpr std::string_view kPlanBackupsFolderName = "backups";
inline constexpr std::string_view kWorldLockFileName = "world.lock";
inline constexpr std::string_view kSnapshotsFolderName = "snapshots";
inline constexpr std::string_view kTransactionFileName = "transaction.json";
/// The plans kept in backups/ (ADR-0010 §9), the newest.
inline constexpr std::size_t kPlanBackupsKept = 10;
/// How long a writer waits for the world lock by default.
inline constexpr std::chrono::milliseconds kWorldLockWait{2000};
/// The meta sidecar's format.
inline constexpr std::int64_t kPlanMetaSchemaVersion = 1;

/// The paths of one world's files.
struct WorldFiles {
    std::filesystem::path folder;
    std::filesystem::path plan;
    std::filesystem::path meta;
    std::filesystem::path overrides;
    std::filesystem::path backups;
    std::filesystem::path lock;
    /// backups/snapshots/ and the journal (world_snapshots.hpp).
    std::filesystem::path snapshots;
    std::filesystem::path transaction;

    /// The files of the world kept in `folder`.
    [[nodiscard]] static WorldFiles in(const std::filesystem::path& folder);

    bool operator==(const WorldFiles&) const = default;
};

/// The files of `library`'s world: <userDataRoot>/worlds/<library id>/.
[[nodiscard]] WorldFiles worldFiles(const std::filesystem::path& userDataRoot, const catalog::LibraryId& library);

/// The world lock, held while the object lives (move-only).
class WorldLock {
public:
    /// Creates the world folder and the lock file when missing and locks the file exclusively, trying again until
    /// `wait` has passed while another holder (another process, or another WorldLock of this one) keeps it; then
    /// removes the temporary files that an interrupted write left in the folder, in backups/ and in the snapshots
    /// ("<name>.tmp-<pid>-<n>", core::writeFileAtomic), rolls back the explicit action a journal names
    /// (world_snapshots.hpp: its snapshot restored, logged) and removes the snapshot folders an interrupted action left
    /// half made or half removed. Errors: Conflict when the lock is still held after `wait`; IoError or Unavailable
    /// when the folder or the lock file cannot be created or opened; those of the rollback (a journal or its snapshot
    /// unreadable, a restore that fails), whose message names transaction.json, the snapshot and how to repair the
    /// world by hand: nothing is written into the world until it is repaired.
    [[nodiscard]] static Expected<WorldLock> acquire(const WorldFiles& files,
                                                     std::chrono::milliseconds wait = kWorldLockWait);

    WorldLock(WorldLock&& other) noexcept;
    WorldLock& operator=(WorldLock&& other) noexcept;
    WorldLock(const WorldLock&) = delete;
    WorldLock& operator=(const WorldLock&) = delete;
    /// Releases the lock and closes the file (which stays in place).
    ~WorldLock();

    [[nodiscard]] const WorldFiles& files() const noexcept { return m_files; }
    /// False once moved from.
    [[nodiscard]] bool held() const noexcept { return m_handle != nullptr; }
    /// The temporary files of interrupted writes removed when the lock was taken.
    [[nodiscard]] const std::vector<std::filesystem::path>& staleFilesRemoved() const noexcept { return m_removed; }
    /// The snapshot (its folder's name) restored when the lock was taken, the rollback of an interrupted action.
    [[nodiscard]] const std::optional<std::string>& rolledBack() const noexcept { return m_rolledBack; }

private:
    WorldLock(void* handle, WorldFiles files) noexcept : m_handle(handle), m_files(std::move(files)) {}
    void release() noexcept;

    void* m_handle = nullptr; ///< a Win32 HANDLE; null once moved from
    WorldFiles m_files;
    std::vector<std::filesystem::path> m_removed;
    std::optional<std::string> m_rolledBack;
};

/// A plan read from its file, with the SHA-256 of the file's bytes (its digest).
struct StoredPlan {
    WorldPlan plan;
    std::string digest;
};

/// Reads a plan file. Errors: NotFound (no plan there), those of reading the file, and readPlan()'s: ValidationFailed
/// for a text that is not exactly the canonical text of a plan (edited by hand or by another program), Unsupported for
/// a newer schemaVersion (written by a newer Retroverse).
[[nodiscard]] Expected<StoredPlan> readStoredPlan(const std::filesystem::path& planFile);

/// The meta sidecar: what a plan's write was, beside the plan.
struct PlanMeta {
    std::int64_t schemaVersion = kPlanMetaSchemaVersion;
    /// When it was written (UTC, ISO-8601) and by which program ("retroverse-cli worldplan update", with
    /// core::versionBanner()).
    std::string writtenAt;
    std::string writer;
    /// How long the generation or update took, in microseconds.
    std::int64_t durationUs = 0;
    /// The plan's digest (the SHA-256 of world-plan.json as written).
    std::string planDigest;
    /// The change report's summary (generation.hpp).
    GenerationOutcome outcome = GenerationOutcome::Generated;
    RebuildLevel rebuild = RebuildLevel::Full;
    std::uint64_t placements = 0;
    std::uint64_t added = 0;
    std::uint64_t kept = 0;
    std::uint64_t unplaced = 0;
    std::uint64_t fixturesAdded = 0;
    std::uint64_t fixturesRemoved = 0;
    std::uint64_t orphanedPins = 0;
    std::uint64_t needsConfirmation = 0;
    std::string digestBefore;

    bool operator==(const PlanMeta&) const = default;
};

/// The sidecar's text: formatted JSON, two-space indents, keys sorted, a final line feed.
[[nodiscard]] std::string writePlanMeta(const PlanMeta& meta);
/// Reads a sidecar back. Errors (context and line): ParseError, ValidationFailed (a key missing, unknown or of another
/// type), Unsupported (a newer schemaVersion).
[[nodiscard]] Expected<PlanMeta> readPlanMeta(std::string_view text, std::string_view context);

struct SaveOptions {
    /// The program writing (PlanMeta::writer) and how long its generation or update took.
    std::string writer;
    std::chrono::microseconds duration{0};
    /// The time of the meta sidecar and of the backup's name; empty: the system clock.
    std::function<std::chrono::system_clock::time_point()> clock;
};

/// What savePlan() did.
struct SaveReport {
    /// False when the folder's plan was that text already: nothing was written, no backup made.
    bool written = false;
    /// The digest of the plan saved (the SHA-256 of its text).
    std::string digest;
    /// The backup of the plan replaced, and the older backups removed beyond kPlanBackupsKept.
    std::optional<std::filesystem::path> backup;
    std::vector<std::filesystem::path> backupsRemoved;
    /// Set when the plan was written but its meta sidecar could not be (the plan is whole and saved; the sidecar is
    /// informative and is written again with the next plan).
    std::optional<Error> metaError;
};

/// Saves `result`'s plan as the world's plan, under `lock` (the folder's): the plan replaced, when there is one and its
/// text differs, is first copied into backups/ (the newest kPlanBackupsKept kept), then the new text is written
/// atomically (core::writeTextFileAtomic: flushed, then renamed), then the meta sidecar. An interruption at any point
/// leaves the previous plan or the new one, whole. A plan whose text is the folder's already is not written again (no
/// backup, the sidecar untouched). Errors: InvalidArgument for a lock moved from, and for a
/// RequiresExplicitRegeneration result (nothing to save);
/// ValidationFailed for a plan that breaks a hard rule of its validation (never saved, ADR-0010 §11); those of
/// writePlan(), of the backup and of the plan's write.
[[nodiscard]] Expected<SaveReport> savePlan(const WorldLock& lock, const GenerationResult& result,
                                            const SaveOptions& options = {});

namespace detail {
/// savePlan() of `result` whose text writePlan() made already (`text`): commitWorld() serialises the plan once.
[[nodiscard]] Expected<SaveReport> savePlanText(const WorldLock& lock, const GenerationResult& result,
                                                const std::string& text, const SaveOptions& options);
} // namespace detail

/// What saveOverrides() did.
struct OverridesSaveReport {
    /// False when the folder held that text already, or held no file and the overrides are empty: nothing written.
    bool written = false;
    /// The SHA-256 of the text saved; empty when no file is there (nothing written, no pin).
    std::string digest;
};

/// Saves `overrides` as the world's overrides.json, under `lock` (the folder's): writeOverrides()'s text written
/// atomically, unless the file holds it already, or there is no file and no pin (a world without overrides keeps no
/// file). An explicit action saves the overrides and the plan together through commitWorld() (world_snapshots.hpp),
/// which calls this. Errors: InvalidArgument for a lock moved from and for overrides writeOverrides() refuses; those of
/// reading the file there and of the write.
[[nodiscard]] Expected<OverridesSaveReport> saveOverrides(const WorldLock& lock, const WorldOverrides& overrides);

} // namespace rv::world
