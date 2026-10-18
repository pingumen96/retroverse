#pragma once

// Rotating backups of a file (ADR-0010 §9: the last 10 world plans before each write): copies named by UTC time in a
// folder of their own, the newest kept. The names follow the catalog database's backups (catalog_db::listBackups()):
// "<stem>-<YYYYMMDDTHHMMSS.mmmZ>[-<n>]<extension>", n from 2 when two backups share a millisecond.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::core {

struct BackupOptions {
    /// The backups kept in the folder, the newest; 0 is taken as 1 (the one just written always stays).
    std::size_t keep = 10;
    /// The time a backup's name holds; empty: the system clock.
    std::function<std::chrono::system_clock::time_point()> clock;
};

/// What backUpFile() did.
struct BackupReport {
    /// The backup written.
    std::filesystem::path backup;
    /// The older backups removed (BackupOptions::keep were kept).
    std::vector<std::filesystem::path> removed;
};

/// Copies `file` into `folder` (created when missing) as a new backup, written atomically (writeFileAtomic(): a backup
/// is whole or absent), then removes the backups of the same name beyond the newest `keep`, ordered by the time and
/// counter their names hold, the one just written always kept (a clock set back would have dated it the oldest). Only
/// files named as backups of `file` count; anything else in the folder is left alone. A backup that cannot be removed
/// is logged and left. Errors: those of reading `file` (NotFound when there is nothing to back up) and of the write;
/// Conflict when the millisecond's names are all taken (1,000 of them).
[[nodiscard]] Expected<BackupReport> backUpFile(const std::filesystem::path& file, const std::filesystem::path& folder,
                                                const BackupOptions& options = {});

/// The backups of `file` (by its name) in `folder`, newest first; none when the folder does not exist.
[[nodiscard]] std::vector<std::filesystem::path> listBackupsOf(const std::filesystem::path& file,
                                                               const std::filesystem::path& folder);

} // namespace rv::core
