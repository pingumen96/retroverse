#pragma once

// GPU-free (rv_library, ADR-0018 §4): the drives and folders the wizard's folder browser shows, and the access test of a
// root. Each may wait as long as the volume makes it wait: never on the main thread. Reads names only, writes nothing.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/esde/discovery.hpp>

namespace rv::library {

/// A row of the browser: a drive or a folder.
struct ListedFolder {
    std::filesystem::path path;
    /// For people (UTF-8, unpaired surrogates as U+FFFD).
    std::string name;

    bool operator==(const ListedFolder&) const = default;
};

/// The most folders one listing returns.
inline constexpr std::size_t kMaxListedFolders = 4000;

/// The machine's drives, by letter. Asks nothing of the volumes: an empty card reader is listed, and fails when entered.
[[nodiscard]] std::vector<ListedFolder> listDrives();

/// The folders of `folder`, by name ignoring ASCII case, at most kMaxListedFolders, Windows' own hidden bookkeeping
/// ($RECYCLE.BIN, System Volume Information) left out. Errors: NotFound (the folder is gone), Unavailable (its drive
/// cannot be reached), IoError (it cannot be listed).
[[nodiscard]] Expected<std::vector<ListedFolder>> listFolders(const std::filesystem::path& folder);

/// Whether a root can be read: esde::checkFolder(), then a look at its first entry (a folder may exist and refuse its
/// listing). Available, or why not.
[[nodiscard]] esde::FolderCheck checkAccess(const std::filesystem::path& folder);

} // namespace rv::library
