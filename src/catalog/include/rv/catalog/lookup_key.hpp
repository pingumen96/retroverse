#pragma once

// Lookup keys (ADR-0008 §4): how the catalog finds a file again. A key is the path of a file or folder relative to its
// system's ROM folder, upper-cased as the Windows file systems compare names, so that the spellings NTFS and exFAT hold
// equal have one key. Identities match on the exact relative path first, then on the key; a new drive letter or ROM
// root changes neither.

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <rv/core/expected.hpp>

namespace rv::catalog {

/// The version of the key rule of makeLookupKey(), stored with the keys: when it changes, or when the casing table does
/// (LookupKeyVersion::casingTable), stored keys are computed again from their exact relative paths, in place.
inline constexpr std::uint32_t kLookupKeyRule = 1;

/// What a key was computed with.
struct LookupKeyVersion {
    std::uint32_t rule = kLookupKeyRule;
    /// core::casingTableVersion() of the system that computed it.
    std::string casingTable;

    auto operator<=>(const LookupKeyVersion&) const = default;
};

/// The version of the keys makeLookupKey() computes on this system: kLookupKeyRule and core::casingTableVersion().
[[nodiscard]] LookupKeyVersion currentLookupKeyVersion();

class LookupKey;

/// The lookup key of `relativePath`, the path of a file or folder relative to its system's ROM folder as found on disk
/// (core::TreeEntry::relativePath) or in a gamelist (after core::resolveRelativePath): '/' between the elements, no
/// "./". The key is that path upper-cased with core::fileSystemUpperCase (LCMapStringEx, the invariant locale's
/// file-system casing, code unit by code unit), never Unicode-normalized: NFC and NFD forms of a name are two names on
/// disk and keep two keys. Names are kept exactly, trailing dots and spaces included, and so is every character the
/// file systems allow (DEL, U+007F, among them). Errors (InvalidArgument, naming the path): an empty path, malformed
/// UTF-8, a character below U+0020 (which NTFS and exFAT refuse in names), '\' or ':', a leading or trailing '/', an
/// empty, "." or ".." element; PlatformError when the system cannot change the case.
[[nodiscard]] Expected<LookupKey> makeLookupKey(std::string_view relativePath);

/// A lookup key with the exact relative path it was computed from (makeLookupKey()). Ordered by key, then by relative
/// path, both ordinal (the UTF-8 bytes): the order of the catalog's entries within a system, in which two names that
/// fold to one key stay apart and in a fixed order. A default-constructed LookupKey is empty and stands for "no path"
/// (a gamelist entry outside its system's folder).
class LookupKey {
public:
    LookupKey() = default;

    /// The key ("COLLECTION A/POCKET PUZZLE (WORLD).GB").
    [[nodiscard]] const std::string& key() const noexcept { return m_key; }
    /// The exact relative path ("Collection A/Pocket Puzzle (World).gb").
    [[nodiscard]] const std::string& relativePath() const noexcept { return m_relativePath; }
    [[nodiscard]] bool empty() const noexcept { return m_relativePath.empty(); }

    auto operator<=>(const LookupKey&) const = default;

private:
    friend Expected<LookupKey> makeLookupKey(std::string_view relativePath);

    LookupKey(std::string key, std::string relativePath)
        : m_key(std::move(key)), m_relativePath(std::move(relativePath)) {}

    // Declared in this order for the defaulted comparison: key first.
    std::string m_key;
    std::string m_relativePath;
};

} // namespace rv::catalog
