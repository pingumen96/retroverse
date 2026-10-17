#pragma once

// File-system situations the walker and the importer must survive, made in a test's temporary folder without
// administrator rights: junctions, a folder that cannot be listed, hidden files, paths longer than MAX_PATH.

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <rv/core/expected.hpp>

namespace rv::test {

/// A junction (a mount point reparse point, which needs no administrator rights): `link`, a new empty folder, leads to
/// the folder `target` (made absolute). Removed, the link alone and never what it leads to, when the object goes:
/// declare it after the TempDir holding it.
class Junction {
public:
    [[nodiscard]] static Expected<Junction> create(const std::filesystem::path& link,
                                                   const std::filesystem::path& target);

    Junction(Junction&& other) noexcept;
    Junction& operator=(Junction&&) = delete;
    Junction(const Junction&) = delete;
    Junction& operator=(const Junction&) = delete;
    ~Junction();

    [[nodiscard]] const std::filesystem::path& link() const noexcept { return m_link; }

private:
    explicit Junction(std::filesystem::path link) : m_link(std::move(link)) {}
    std::filesystem::path m_link;
};

/// While alive, `folder` cannot be listed by this user: its DACL is replaced by a protected one that grants the user
/// everything but FILE_LIST_DIRECTORY. The destructor puts the folder's permissions back (its owner may always).
class ListingDenied {
public:
    [[nodiscard]] static Expected<ListingDenied> apply(const std::filesystem::path& folder);

    ListingDenied(ListingDenied&& other) noexcept;
    ListingDenied& operator=(ListingDenied&&) = delete;
    ListingDenied(const ListingDenied&) = delete;
    ListingDenied& operator=(const ListingDenied&) = delete;
    ~ListingDenied();

private:
    explicit ListingDenied(std::filesystem::path folder) : m_folder(std::move(folder)) {}
    std::filesystem::path m_folder;
};

/// While alive, the data of the file it is applied to cannot be read by this user (I2 step 11a: a first import reads no
/// ROM byte): the same protected DACL as ListingDenied's, since FILE_READ_DATA is FILE_LIST_DIRECTORY's bit. The file's
/// attributes, size and times stay readable (FILE_READ_ATTRIBUTES), its folder listable, and opening it for reading
/// (GENERIC_READ) fails with access denied. The destructor puts the file's permissions back.
using DataReadDenied = ListingDenied;

/// Sets or clears FILE_ATTRIBUTE_HIDDEN.
[[nodiscard]] Status setHidden(const std::filesystem::path& path, bool hidden);

/// Creates the file `path` with `content`, and the folders above it, through \\?\ paths: any length, whatever the
/// system's long path setting.
[[nodiscard]] Status writeFileAnyLength(const std::filesystem::path& path, std::string_view content);

/// Creates the folder `path` and the folders above it through \\?\ paths (any length); existing ones are kept.
[[nodiscard]] Status createFoldersAnyLength(const std::filesystem::path& path);

/// Sets the last write time of the file `path` through its \\?\ form (any length).
[[nodiscard]] Status setLastWriteTimeAnyLength(const std::filesystem::path& path, std::filesystem::file_time_type time);

/// Removes the tree `path` through its \\?\ form (any length); links are removed, never followed.
void removeTreeAnyLength(const std::filesystem::path& path);

} // namespace rv::test
