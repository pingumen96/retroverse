#pragma once

// Systems as the catalog keeps them (ADR-0008 §3, §4): what es_systems.xml says of a system that the catalog shows and
// stores, where its ROM folder is, whether that folder could be listed, whether its gamelist could be read and the
// command its gamelist chooses. How ES-DE launches a system's games (its extensions and commands) stays with the
// adapter (rv_esde, launch translation).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/ids.hpp>

namespace rv::catalog {

/// Whether a system's gamelist was read (ADR-0008 §7).
enum class GamelistState : std::uint8_t {
    Read,   ///< read and parsed: the snapshot holds its metadata
    Absent, ///< the system has no gamelist: its files are unscraped
    Failed, ///< it exists but could not be read or parsed, even after a retry (cut short or padded with NUL characters
            ///< while ES-DE writes it, empty, no <gameList>): the snapshot holds none of its metadata, and the import
            ///< keeps the system's previous rows (on a first import the system is "metadata pending", not unscraped)
};

/// "read", "absent", "failed".
[[nodiscard]] constexpr std::string_view toString(GamelistState state) noexcept {
    switch (state) {
    case GamelistState::Read:
        return "read";
    case GamelistState::Absent:
        return "absent";
    case GamelistState::Failed:
        return "failed";
    }
    return "unknown";
}
[[nodiscard]] constexpr std::optional<GamelistState> parseGamelistState(std::string_view text) noexcept {
    return detail::enumFromString(text, GamelistState::Failed);
}

/// A system of the library.
struct SystemInfo {
    /// <name>.
    SystemName name;
    /// <fullname> ("Nintendo Game Boy"): what lists and filters show.
    std::string fullName;
    /// <platform> split at its commas, each trimmed, in order ("pc, pcwindows" gives "pc" and "pcwindows").
    std::vector<std::string> platforms;
    /// <theme>.
    std::string theme;
    /// <path> exactly as es_systems.xml writes it ("%ROMPATH%\gb").
    std::string romFolderConfigured;
    /// The ROM folder as the adapter resolved it (absolute; %ROMPATH% expanded with ROMDirectory, or with the
    /// library's ROM root override), as ES-DE builds it: from the system's name, whatever the case on disk.
    std::filesystem::path romFolder;
    /// Whether romFolder could be listed: Available or SourceUnavailable (with the reason).
    Availability availability = Availability::Available;
    SourceUnavailableReason unavailableReason = SourceUnavailableReason::None;
    GamelistState gamelist = GamelistState::Absent;
    /// The <label> of the gamelist's top-level <alternativeEmulator>, as written: the ES-DE command label the system
    /// launches with when neither the game nor Retroverse chooses one (ADR-0008 §8). Empty when the gamelist names none
    /// or was not read (GamelistState::Failed: the import keeps the value it stored, as the system's other rows).
    std::string alternativeEmulator;

    bool operator==(const SystemInfo&) const = default;
};

} // namespace rv::catalog
