#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// A game as a screen sees it (I3 step 11; since I5 step 1 apart from screen_policy.hpp, which includes it, so that the
// playlist can use it): its media, the data its playlist orders it by, and its generated title card's text.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/mip_chain.hpp>

namespace rv::app {

/// The media of a game the hero CRT can show: absolute paths, empty where the game has none.
struct ScreenMedia {
    std::filesystem::path video;
    std::filesystem::path screenshot;
    std::filesystem::path titleScreen;
    std::filesystem::path cover;

    bool operator==(const ScreenMedia&) const = default;
};

/// The generated title card at the end of a game's fallback chain (I5 step 1, ADR-0016 §8): its title (and the title of
/// its file name, for a title the font cannot draw), year and system's full name, drawn on its room's theme pack's label
/// palette (empty: assets::kDefaultLabelPalette) in the colour `seed` picks (the placement's seed, so that the card
/// takes its item's generated label's colour). A card without a title is none: the game has no card.
struct ScreenCard {
    std::string title;
    std::string fallbackTitle;
    std::string year;
    std::string systemName;
    std::uint64_t seed = 0;
    std::vector<assets::Rgb8> palette;

    [[nodiscard]] bool empty() const noexcept { return title.empty() && fallbackTitle.empty(); }

    bool operator==(const ScreenCard&) const = default;
};

/// A game the hero CRT may play: its catalog id (ui::CatalogEntry::id, the GameId's text), its media and when it was
/// last played (any clock whose larger values are later, such as seconds since 1970; nothing: never). A game that is
/// no start candidate is played only when selected (the generated room's games it does not place, I3 step 10c: the
/// start choice is a placed game, ADR-0012 §7). Since I5 step 1 its playlist's data (the room's sort key, a favourite)
/// and its title card.
struct ScreenGame {
    std::string id;
    ScreenMedia media;
    std::optional<std::int64_t> lastPlayed;
    bool startCandidate = true;
    std::string sortKey;
    bool favorite = false;
    ScreenCard card;

    bool operator==(const ScreenGame&) const = default;
};

/// The video extensions ES-DE reads (esde::kVideoExtensions, kept equal by a test: rv_app_cli does not link rv_esde).
inline constexpr std::string_view kScreenVideoExtensions[] = {".mp4", ".mkv", ".avi", ".wmv", ".mov", ".webm"};

/// True when `path` names a file ES-DE would play: one of kScreenVideoExtensions, in any case. Others (an `.flv`) are
/// never opened (ADR-0012 §7).
[[nodiscard]] bool isPlayableVideoFile(const std::filesystem::path& path);

} // namespace rv::app
