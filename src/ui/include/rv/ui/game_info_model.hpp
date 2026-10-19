#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// Informazioni (I3 step 14b; ADR-0012 §5, specification §12.1): a game's description and metadata (platform, year,
// developer, publisher, genre, players) and the other versions of its title in the catalog with their regions, over
// the world's card or the quick catalog. Up and Down scroll the description, the triggers and Page Up / Page Down by a
// page, Home and End to its ends; Back closes it, Vedi media (the pad's Y) opens the media viewer of the same game.
// Every sentence is the document's (assets/ui/game_info.rml): the code passes catalog data as it comes.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

/// The ids and the data model the document (assets/ui/game_info.rml) and its screen (rv::ui::GameInfoScreen) share
/// (tests/unit/ui/test_media_viewer.cpp checks the document against them).
namespace game_info {
inline constexpr std::string_view kDocumentPath = "ui/game_info.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/game_info.rcss";
inline constexpr std::string_view kDataModel = "game_info";
inline constexpr std::string_view kInfoId = "game-info";
inline constexpr std::string_view kTitleId = "game-info-title";
/// The scrolled region (the description and the versions).
inline constexpr std::string_view kBodyId = "game-info-body";
inline constexpr std::string_view kDescriptionId = "game-info-description";
inline constexpr std::string_view kVersionsId = "game-info-versions";
inline constexpr std::string_view kCloseId = "info-close";
inline constexpr std::string_view kMediaId = "info-media";
/// The data event the buttons send: "act('<close|media>')".
inline constexpr std::string_view kActEvent = "act";
/// The most other versions it lists (the document says how many more there are).
inline constexpr std::size_t kMaxVersions = 12;
} // namespace game_info

/// Another version of the game's title in the catalog (ADR-0010's duplicate-title entries and their representative).
struct OtherVersion {
    /// Its GameId's text form.
    std::string gameId;
    /// The catalog's title ("" when the catalog lacks it: the document says so).
    std::string title;
    /// Its region tag as its file name writes it ("USA", "Europe", "JU"); empty without one.
    std::string region;

    bool operator==(const OtherVersion&) const = default;
};

/// What Informazioni shows: catalog data as it comes.
struct GameInfoContent {
    std::string gameId;
    /// "" when unknown (the document says "Gioco senza titolo").
    std::string title;
    std::string platform;
    /// 0 when unknown.
    int year = 0;
    std::string developer;
    std::string publisher;
    std::string genre;
    std::string players;
    /// The <desc> as written (line feeds kept); empty: the document says there is none.
    std::string description;
    /// The catalog answered for the game (false: only the title is known).
    bool details = false;
    /// The other versions, at most game_info::kMaxVersions, and how many there are in all.
    std::vector<OtherVersion> versions;
    std::uint32_t versionCount = 0;

    bool operator==(const GameInfoContent&) const = default;
};

/// What a command does on Informazioni.
enum class InfoStep : std::uint8_t {
    None,
    ScrollUp,   ///< Up: a line up
    ScrollDown, ///< Down: a line down
    PageUp,     ///< Page Up, the left trigger, the left shoulder
    PageDown,   ///< Page Down, the right trigger, the right shoulder
    Top,        ///< Home
    Bottom,     ///< End
    Close,      ///< Back
    Media,      ///< Vedi media: the viewer of the same game
};

/// "none", "scroll-up", "scroll-down", "page-up", "page-down", "top", "bottom", "close", "media".
[[nodiscard]] std::string_view toString(InfoStep step) noexcept;
[[nodiscard]] InfoStep infoStepOf(UiCommand command) noexcept;

/// The scroll offset after `step` from `top` in a region `visible` high whose content is `content` high (any unit):
/// a line is `line`, a page the region's height less a line; clamped to [0, content - visible].
[[nodiscard]] float scrolledTop(float top, InfoStep step, float visible, float content, float line) noexcept;

} // namespace rv::ui
