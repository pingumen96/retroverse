#pragma once

// A library (ADR-0008 §4, §6): its fixed id, the adapter that reads it and the folders the adapter needs. library.json
// under <user data>/config names it; the database is the authority on its id, and a mismatch between the two is refused
// until the user re-associates them explicitly.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include <rv/catalog/ids.hpp>

namespace rv::catalog {

/// How a library is read.
enum class LibraryAdapter : std::uint8_t {
    Esde, ///< an ES-DE installation (rv_esde)
};

/// "esde".
[[nodiscard]] constexpr std::string_view toString(LibraryAdapter adapter) noexcept {
    switch (adapter) {
    case LibraryAdapter::Esde:
        return "esde";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<LibraryAdapter> parseLibraryAdapter(std::string_view text) noexcept {
    return detail::enumFromString(text, LibraryAdapter::Esde);
}

/// A library profile: what library.json holds (the user's choices, confirmed after discovery).
struct LibraryProfile {
    LibraryId id;
    LibraryAdapter adapter = LibraryAdapter::Esde;
    /// The ES-DE home: the folder holding settings/, gamelists/ and custom_systems/, which ES-DE always names "ES-DE"
    /// inside its home folder: %USERPROFILE%\ES-DE by default, <folder>\ES-DE with --home <folder>, the ES-DE folder
    /// beside ES-DE.exe in a portable installation. ES-DE's `~` (in ROMDirectory, MediaDirectory, find rules and
    /// commands) is that home folder, this folder's parent (esdeHome.parent_path()), not this folder (ADR-0008 §8).
    std::filesystem::path esdeHome;
    /// The folder of ES-DE.exe and its resources/ (es_systems.xml, es_find_rules.xml, the MAME lists); nothing when not
    /// known, which leaves %ESPATH% unresolved (ADR-0008 §8).
    std::optional<std::filesystem::path> installFolder;
    /// Replaces ES-DE's ROMDirectory when set.
    std::optional<std::filesystem::path> romRoot;
    /// Replaces ES-DE's MediaDirectory when set.
    std::optional<std::filesystem::path> mediaRoot;

    bool operator==(const LibraryProfile&) const = default;
};

} // namespace rv::catalog
