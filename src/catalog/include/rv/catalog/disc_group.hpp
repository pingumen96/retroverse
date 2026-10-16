#pragma once

// Disc groups (ADR-0008 §3): a .cue or .m3u that is itself a game of its system, and the files it references, which are
// its members (DiscMember entries) instead of games. ES-DE lists every track of a .cue as a game; the catalog keeps
// them identifiable but hidden behind their head. A chain (an .m3u naming cue sheets) gives a group per head.

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/lookup_key.hpp>

namespace rv::catalog {

/// What the head of a group is.
enum class DiscGroupKind : std::uint8_t {
    Cue, ///< a cue sheet: its members are the files of its FILE lines (the tracks)
    M3u, ///< a playlist: its members are the files it lists (the discs, possibly cue sheets with tracks of their own)
};

/// "cue", "m3u".
[[nodiscard]] constexpr std::string_view toString(DiscGroupKind kind) noexcept {
    switch (kind) {
    case DiscGroupKind::Cue:
        return "cue";
    case DiscGroupKind::M3u:
        return "m3u";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<DiscGroupKind> parseDiscGroupKind(std::string_view text) noexcept {
    return detail::enumFromString(text, DiscGroupKind::M3u);
}

/// One group: a head and its members, all entries of one system.
struct DiscGroup {
    SystemName system;
    LookupKey head;
    DiscGroupKind kind = DiscGroupKind::Cue;
    /// The files the head references that exist as entries of the system, in the order the head names them, each once
    /// (names matched with the key's case folding).
    std::vector<LookupKey> members;

    bool operator==(const DiscGroup&) const = default;
};

/// A disc group as an import stored it, with the size and last write time its head had when the group was read: the
/// cache ADR-0008 §3 asks for (membership cached by key, size and modification time, and kept when a cue sheet cannot
/// be read). The import keeps the groups of each import and hands them to the next load
/// (ILibrarySource, LibraryLoadOptions::discGroups).
struct CachedDiscGroup {
    DiscGroup group;
    /// The head's FileFacts::size and FileFacts::lastWriteTime (FILETIME ticks) when the group was read.
    std::uint64_t headSize = 0;
    std::int64_t headLastWriteTime = 0;

    bool operator==(const CachedDiscGroup&) const = default;
};

} // namespace rv::catalog
