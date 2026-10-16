#pragma once

// A walker for large folder trees (ES-DE's ROM and media folders: hundreds of thousands of files, paths longer than
// MAX_PATH, links), deterministic and never fatal below its root (ADR-0008 §3, §4).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::core {

enum class TreeEntryKind {
    File,
    Directory,
    /// Something below the root that could not be walked: a folder that cannot be listed (Unavailable, whatever the
    /// reason: access denied, a drive gone meanwhile...), a link that would loop (Unsupported) or whose target cannot
    /// be opened, a name that does not convert to UTF-8 (InvalidArgument). Reported after the entry it concerns (if
    /// any); what lies below it is skipped and the walk goes on. Whatever its code, an Error entry after a Directory
    /// entry means that the folder's content is unknown: "source unavailable" for the catalog (ADR-0008 §4), never
    /// deleted files.
    Error,
};

/// One entry of a walk.
struct TreeEntry {
    TreeEntryKind kind = TreeEntryKind::File;
    /// The path below the root: UTF-8, '/' between the elements, the names as the file system returns them (for an
    /// Error entry whose name does not convert, its unpaired surrogates as U+FFFD). Reopen the entry through
    /// extendedLengthPathBelow(root, relativePath): Windows would normalize a plain root / relativePath (names that end
    /// with a dot or a space, device names such as "NUL").
    std::string relativePath;
    /// Size in bytes; 0 for a folder. For a symbolic link to a file that is followed, its target's.
    std::uint64_t size = 0;
    /// Last write time as the file system records it (FILETIME ticks); for a followed link to a file, its target's.
    std::filesystem::file_time_type lastWriteTime{};
    /// FILE_ATTRIBUTE_HIDDEN and FILE_ATTRIBUTE_SYSTEM of the entry itself (a link's own). A name with a leading dot
    /// is not marked: whether it counts as hidden is the caller's rule.
    bool hidden = false;
    bool system = false;
    /// The reparse tag of a reparse point (IO_REPARSE_TAG_MOUNT_POINT for a junction, IO_REPARSE_TAG_SYMLINK, cloud
    /// file tags...); 0 for an ordinary file or folder.
    std::uint32_t reparseTag = 0;
    /// For an Error entry: the code (NotFound, Unavailable, IoError, Unsupported for a link loop...) and the reason.
    Error error;
};

/// What the walker does after an entry.
enum class WalkAction {
    Continue,
    /// For a folder: do not enter it.
    SkipChildren,
    /// End the walk now (TreeWalkSummary::stopped).
    Stop,
};

using TreeVisitor = std::function<WalkAction(const TreeEntry& entry)>;

struct TreeWalkOptions {
    /// Enter junctions and symbolic links to folders, and report symbolic links to files with their target's size and
    /// time, unless a folder link leads back to a folder of the path walked down to it (the same volume serial number
    /// and file id: a loop, reported as an Error entry). Off: links are reported as they are listed and never entered.
    bool followLinks = true;
    /// Read before each entry: once it holds true the walk ends (TreeWalkSummary::cancelled).
    const std::atomic<bool>* cancel = nullptr;
};

struct TreeWalkSummary {
    std::uint64_t files = 0;
    std::uint64_t directories = 0;
    std::uint64_t errors = 0;
    bool cancelled = false; ///< TreeWalkOptions::cancel ended the walk
    bool stopped = false;   ///< the visitor ended the walk (WalkAction::Stop)
};

/// Walks the folder tree under `root` (absolute or relative to the current directory, any length: listed through its
/// \\?\ form with FindFirstFileExW, FindExInfoBasic and FIND_FIRST_EX_LARGE_FETCH; no file is opened, only folders
/// whose links are checked and the targets of file links) and calls `visit` for every entry below it, depth first: the
/// entries of a folder in the ordinal order of their UTF-8 names, each folder's content right after the folder. Below
/// the root nothing is fatal (TreeEntryKind::Error). Errors, for the root only: NotFound (no such folder),
/// InvalidArgument (not a folder), Unavailable (its drive or share cannot be reached, or it cannot be listed: access
/// denied, ...).
[[nodiscard]] Expected<TreeWalkSummary> walkTree(const std::filesystem::path& root, const TreeVisitor& visit,
                                                 const TreeWalkOptions& options = {});

/// walkTree() into a vector, every entry visited (small trees, tests).
[[nodiscard]] Expected<std::vector<TreeEntry>> listTree(const std::filesystem::path& root,
                                                        const TreeWalkOptions& options = {});

} // namespace rv::core
