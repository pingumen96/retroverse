#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::core {

inline constexpr std::uintmax_t kDefaultTextLimitBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uintmax_t kDefaultBinaryLimitBytes = 1024ull * 1024ull * 1024ull;

/// Reads a whole file. Files larger than `maxBytes` are rejected (untrusted inputs, spec §19.1). Errors: NotFound (no
/// such file), Unavailable (its drive or share cannot be reached), InvalidArgument (not a regular file), OutOfRange
/// (too large), IoError (anything else). Never throws, whatever the path holds.
[[nodiscard]] Expected<std::string> readTextFile(const std::filesystem::path& path,
                                                 std::uintmax_t maxBytes = kDefaultTextLimitBytes);
[[nodiscard]] Expected<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path,
                                                              std::uintmax_t maxBytes = kDefaultBinaryLimitBytes);

/// Reads a whole file that another program may be rewriting meanwhile (ES-DE rewrites a gamelist after every game
/// played from it, ADR-0008 §7): opened for reading only, with full sharing (FILE_SHARE_READ | FILE_SHARE_WRITE |
/// FILE_SHARE_DELETE) so that the other program is never blocked, through extendedLengthPath() (paths of any length; a
/// plain path is normalized there, so a name listed by walkTree() is reopened through extendedLengthPathBelow()).
/// Errors: NotFound (no such file or folder), Unavailable (its drive, share or device cannot be reached),
/// InvalidArgument (a folder or a device), OutOfRange (larger than `maxBytes`), Conflict (another program holds the
/// file, or the range read, locked), IoError (any other failure, and a short read: the file shrank while it was read).
/// Never throws.
[[nodiscard]] Expected<std::string> readFileShared(const std::filesystem::path& path, std::uintmax_t maxBytes);

/// A file opened for reading only, with full sharing, as readFileShared() opens it (I2 step 11b: the quick hash of a
/// ROM file reads its two ends), to read parts of it at given offsets: nothing is ever written to it. Move-only; the
/// handle closes when the object goes.
class SharedFileReader {
public:
    /// The file's size and last write time as its open handle reports them (the file record): the last write time in
    /// FILETIME ticks, 100 ns since 1601-01-01 UTC, as walkTree() lists it.
    struct Facts {
        std::uint64_t size = 0;
        std::int64_t lastWriteTime = 0;

        bool operator==(const Facts&) const = default;
    };

    /// Opens `path` through extendedLengthPath() (a \\?\ path, such as extendedLengthPathBelow() gives for a name a
    /// walk listed, is used as it is: names ending with a dot or a space are reached). Errors as readFileShared():
    /// NotFound, Unavailable, InvalidArgument (a folder or a device), Conflict, IoError (access denied included).
    [[nodiscard]] static Expected<SharedFileReader> open(const std::filesystem::path& path);

    SharedFileReader(SharedFileReader&& other) noexcept;
    SharedFileReader& operator=(SharedFileReader&& other) noexcept;
    SharedFileReader(const SharedFileReader&) = delete;
    SharedFileReader& operator=(const SharedFileReader&) = delete;
    ~SharedFileReader();

    /// The size and last write time now. Errors: IoError, Unavailable (the drive went).
    [[nodiscard]] Expected<Facts> facts() const;
    /// Reads exactly `out.size()` bytes from `offset`. Errors: IoError on a short read (the file shrank meanwhile),
    /// Conflict (another program locked the range), Unavailable (the drive went), IoError otherwise.
    [[nodiscard]] Status read(std::uint64_t offset, std::span<std::byte> out) const;

private:
    SharedFileReader(void* handle, std::string context) noexcept : m_handle(handle), m_context(std::move(context)) {}
    void close() noexcept;

    void* m_handle = nullptr; ///< a Win32 HANDLE; null once moved from
    std::string m_context;    ///< the path, for messages
};

/// The \\?\ form of `path` for the Win32 file functions, which then accept it at any length whatever the system's
/// long path setting and never normalize it again: made absolute against the current directory and normalized as
/// Windows does ("." and ".." resolved, '/' turned into '\'), "\\?\" put in front ("\\?\UNC\server\share\..." for a
/// UNC path). A path that has the prefix already, or a device path ("\\.\"), is returned as it is. That normalization
/// also changes names: trailing dots and spaces are dropped from the elements ("Game.gb." becomes "Game.gb") and a
/// device name ("NUL", "CON", "COM1") becomes the device ("\\.\NUL"): use it for folders the user names, and
/// extendedLengthPathBelow() for names read from the file system.
[[nodiscard]] std::wstring extendedLengthPath(const std::filesystem::path& path);

/// The \\?\ form of `relativeUtf8` below `root`, for a name listed by walkTree() (TreeEntry::relativePath) or
/// resolved by resolveRelativePath(): `root` goes through extendedLengthPath(), then each element of `relativeUtf8`
/// ('/' or '\' between them) is appended exactly as written, so that every name the file system holds is reached again
/// ("Game.gb.", "Disc 1 /Track.bin", a file named "NUL"). Errors (InvalidArgument): malformed UTF-8, an empty path or
/// element, a "." or ".." element, ':' (alternate data streams), a NUL character.
[[nodiscard]] Expected<std::filesystem::path> extendedLengthPathBelow(const std::filesystem::path& root,
                                                                      std::string_view relativeUtf8);

/// Writes `data` to a temporary file in the destination directory and renames it over `path`, so readers
/// never observe a partially written file. Parent directories are created when missing. Never throws.
[[nodiscard]] Status writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data);
[[nodiscard]] Status writeTextFileAtomic(const std::filesystem::path& path, std::string_view text);

/// Last modification time, or nothing when the file cannot be queried.
[[nodiscard]] std::optional<std::filesystem::file_time_type> lastWriteTime(const std::filesystem::path& path) noexcept;

} // namespace rv::core
