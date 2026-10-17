#pragma once

// Fingerprints of ROM files (ADR-0008 §4; I2 step 11b). Every import stores each file's size and last write time from
// the listing, which costs nothing (FileFacts). The quick hash is SHA-256 over the first and last 64 KiB of a file,
// computed by a progressive pass (reconciliation.hpp: runQuickHashPass()) and, for the new files the pairing needs, by
// the pairing pass itself; it is stored with the size and last write time of the file it was computed from, and a hash
// whose size or time are no longer the file's is stale (isCurrent()). A first import reads no ROM byte.
//
// The quick hash, exactly (rule kQuickHashRule; scripts/quick_hash_oracle.py computes the golden vectors with Python's
// hashlib, tests/unit/catalog/test_fingerprint.cpp holds them):
//  - a file of n bytes, n <= 131,072 (2 x 64 KiB): SHA-256 of its whole content, each byte once; the empty file hashes
//  to
//    SHA-256 of nothing (e3b0c442...b855);
//  - a file of n > 131,072 bytes: SHA-256 of its bytes [0, 65,536) followed by its bytes [n - 65,536, n);
//  - nothing else enters the digest (neither the size nor the name): the pairing compares sizes before it compares
//    hashes. A folder has no quick hash.
// At most 128 KiB is read per file, opened for reading only with full sharing (core::SharedFileReader); the size and
// time are those of the open handle, taken before the reads and checked again after them, so that a file written
// meanwhile is reported (IoError) rather than hashed half old, half new.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include <rv/catalog/entry.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/sha256.hpp>

namespace rv::catalog {

/// The version of the quick hash's definition above.
inline constexpr std::uint32_t kQuickHashRule = 1;
/// The bytes read at each end of a file larger than kQuickHashWholeFileLimit.
inline constexpr std::uint64_t kQuickHashPartBytes = 64ull * 1024ull;
/// A file of at most this many bytes is hashed whole.
inline constexpr std::uint64_t kQuickHashWholeFileLimit = 2 * kQuickHashPartBytes;

/// A quick hash and the facts of the file it was computed from.
struct QuickHash {
    core::Sha256Digest digest{};
    /// The size of the file hashed, in bytes.
    std::uint64_t size = 0;
    /// The last write time of the file hashed, in FILETIME ticks (FileFacts::lastWriteTime).
    std::int64_t lastWriteTime = 0;

    bool operator==(const QuickHash&) const = default;
};

/// Whether `hash` belongs to the file `file` describes: a file (no folder) of its size and last write time. A hash that
/// does not is stale: the file changed since it was hashed.
[[nodiscard]] bool isCurrent(const QuickHash& hash, const FileFacts& file) noexcept;

/// The bytes the quick hash of a file of `size` bytes reads: the whole file up to kQuickHashWholeFileLimit, else
/// 2 x kQuickHashPartBytes.
[[nodiscard]] constexpr std::uint64_t quickHashBytes(std::uint64_t size) noexcept {
    return size <= kQuickHashWholeFileLimit ? size : kQuickHashWholeFileLimit;
}

/// The quick hash's digest of a file whose whole content is `content`, as defined above (golden vectors).
[[nodiscard]] core::Sha256Digest quickHashDigest(std::span<const std::byte> content) noexcept;

/// The digest as 64 lowercase hexadecimal digits.
[[nodiscard]] std::string toHex(const core::Sha256Digest& digest);

/// Reads the quick hash of the file `file` (any path; a \\?\ path is used as it is), as defined above. Errors:
/// core::SharedFileReader's (NotFound, Unavailable, InvalidArgument for a folder or a device, Conflict for a locked
/// range, IoError for a file denied or written while it was read).
[[nodiscard]] Expected<QuickHash> readQuickHash(const std::filesystem::path& file);

/// Reads the quick hash of the file `relativePath` (as a walk lists it: LookupKey::relativePath()) below `folder` (a
/// system's ROM folder), reached through core::extendedLengthPathBelow(): any length, names ending with a dot or a
/// space included. Errors: InvalidArgument for a relative path that names no file below `folder`, and
/// readQuickHash()'s.
[[nodiscard]] Expected<QuickHash> readQuickHashBelow(const std::filesystem::path& folder,
                                                     std::string_view relativePath);

/// How the reconciliation passes read quick hashes: readQuickHashBelow() unless a test gives its own (to count the
/// files read, to fail some, to cancel in the middle).
using QuickHashReader =
    std::function<Expected<QuickHash>(const std::filesystem::path& folder, std::string_view relativePath)>;

} // namespace rv::catalog
