#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The world's game card (I3 step 14a; ADR-0012 §5, style §16.3): the card beside the inspected item, with its title,
// platform, year, developer, genre, players, the launch readiness and its first blocker (the quick catalog's keys and
// sentences: ui::quick_catalog::kReadinessKeys, kBlockerCodes, one table its tests share), the number of other versions
// in the catalog, the cover (a texture of the application's, "rv-texture:"), and its actions as buttons: Avvia, Vedi
// media, Informazioni, Indietro. Every sentence is the document's (assets/ui/game_card.rml): the code passes data and
// codes. The buttons stand in a 2 x 2 grid (Avvia, Vedi media / Informazioni, Indietro); the arrows and the D-pad move
// the focus within it (no wrap), Enter or the pad's south button presses the focused one, a click presses the one
// clicked; Avvia has the focus whenever the card shows another game.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

/// The ids, the data model and the texture key the card's document (assets/ui/game_card.rml) and its screen
/// (rv::ui::GameCardScreen) share (tests/unit/ui/test_game_card.cpp checks the document against them).
namespace game_card {
inline constexpr std::string_view kDocumentPath = "ui/game_card.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/game_card.rcss";
inline constexpr std::string_view kDataModel = "game_card";
inline constexpr std::string_view kCardId = "game-card";
inline constexpr std::string_view kTitleId = "game-card-title";
/// The cover: an <img> whose source is kCoverSource, the application's texture registered under kCoverTexture
/// (ui::UiSystem::setTexture), so that RmlUi never loads the user's files itself (ADR-0012 §5).
inline constexpr std::string_view kCoverId = "game-card-cover";
inline constexpr std::string_view kTextureScheme = "rv-texture:";
inline constexpr std::string_view kCoverTexture = "game-card-cover";
inline constexpr std::string_view kCoverSource = "rv-texture:game-card-cover";
/// The cover's frame (game_card.rcss's .cover-frame, in dp: 160 x 118 in the compact theme): the front label's aspect,
/// which the 16-bit cartridges' hero pictures have (I3).
inline constexpr std::uint32_t kCoverFrameWidth = 256;
inline constexpr std::uint32_t kCoverFrameHeight = 188;
/// The buttons' region and the buttons, in focus order (CardButton).
inline constexpr std::string_view kActionsId = "game-card-actions";
inline constexpr std::string_view kPlayId = "card-play";
inline constexpr std::string_view kMediaId = "card-media";
inline constexpr std::string_view kInfoId = "card-info";
inline constexpr std::string_view kBackId = "card-back";
/// The data event a button's click sends: "press('<toString(CardButton)>')".
inline constexpr std::string_view kPressEvent = "press";
/// What a launch from the card ended with (the outcome codes of rv_app's launch_outcome, which rv_ui does not link):
/// the card says it in a sentence of its own, else a general one.
inline constexpr std::array<std::string_view, 10> kOutcomeCodes{
    "quick-exit",     "processes-remained", "left-running", "crashed",     "failed-to-start",
    "not-configured", "emulator-missing",   "core-missing", "rom-missing", "not-ready"};
} // namespace game_card

/// The card's buttons, in focus order.
enum class CardButton : std::uint8_t { Play, Media, Info, Back };

inline constexpr std::array<CardButton, 4> kCardButtons{CardButton::Play, CardButton::Media, CardButton::Info,
                                                        CardButton::Back};

/// "play", "media", "info", "back" (the press event's argument, the run report's names).
[[nodiscard]] std::string_view toString(CardButton button) noexcept;
/// Parses toString(CardButton) exactly.
[[nodiscard]] std::optional<CardButton> cardButtonFromName(std::string_view name) noexcept;
/// The button's element id (game_card::kPlayId ...).
[[nodiscard]] std::string_view elementIdOf(CardButton button) noexcept;

/// The focus after moving from `from` in `direction` within the 2 x 2 grid (Avvia, Vedi media on the first row,
/// Informazioni, Indietro on the second): `from` itself at an edge.
[[nodiscard]] CardButton stepFocus(CardButton from, NavDirection direction) noexcept;

/// What the card shows: catalog data shown as it comes (the title, the platform's display name, ...), keys the
/// document labels (readiness, blocker, state, outcome), and counts.
struct GameCardContent {
    /// The game's catalog id (its GameId's text form): a new id puts the focus back on Avvia.
    std::string gameId;
    /// The catalog's title; "" when the catalog lacks the game (the document says "Gioco senza titolo").
    std::string title;
    std::string platform;
    /// 0 when unknown ("n.d.").
    int year = 0;
    std::string developer;
    std::string genre;
    std::string players;
    /// The quick catalog's keys (CatalogEntry::state, ::readiness, ::blocker, ::verified): one of
    /// quick_catalog::kReadinessKeys or empty, one of kBlockerCodes or empty.
    std::string state;
    std::string readiness;
    std::string blocker;
    bool verified = false;
    /// The catalog's details are known (the catalog answered for the game): until then the card shows the title, the
    /// cover and the buttons, and says it is reading the rest.
    bool details = false;
    /// The game's file is not there (the room's item is unavailable).
    bool unavailable = false;
    /// Other versions of the title in the catalog (the plan's duplicate-title entries of the game, ADR-0010 §3).
    std::uint32_t otherVersions = 0;
    /// The cover's texture is registered (the hero texture is resident): else the card shows its frame alone.
    bool cover = false;
    /// The cover's shape, width by height in any unit: the hero picture's, its room's family's label quad (ADR-0013
    /// §4). 0 x 0 is the frame's own shape (the 16-bit cartridges', I3's card).
    std::uint32_t coverWidth = 0;
    std::uint32_t coverHeight = 0;
    /// The last launch from the card did not end normally: its outcome code (game_card::kOutcomeCodes, or another code
    /// the general sentence covers); empty when nothing is to be told.
    std::string outcome;

    bool operator==(const GameCardContent&) const = default;
};

/// Where the cover stands in its frame (game_card::kCoverFrameWidth x kCoverFrameHeight), as fractions of the frame's
/// width (width, left) and height (height, top): the cover at its shape's aspect, as large as the frame holds and
/// centred, never stretched (ADR-0011 §5). The frame's own shape (and 0 x 0) fills it: {1, 1, 0, 0}.
struct CardCoverBox {
    double width = 1.0;
    double height = 1.0;
    double left = 0.0;
    double top = 0.0;

    bool operator==(const CardCoverBox&) const = default;
};
[[nodiscard]] CardCoverBox cardCoverBox(std::uint32_t shapeWidth, std::uint32_t shapeHeight) noexcept;

/// The card's focus and what its input presses. Focus moves only with the directions; a click moves it to the button
/// clicked.
class GameCardModel {
public:
    /// The card shows `gameId` (another game than before: the focus goes back to Avvia).
    void show(std::string_view gameId);
    /// The card is gone: the next show() starts on Avvia whatever the game.
    void hide() noexcept;

    /// A UI command: the directions move the focus (nothing pressed), Activate presses the focused button, Back
    /// Indietro, Media Vedi media, Info Informazioni; other commands do nothing. The button pressed, if any.
    std::optional<CardButton> apply(UiCommand command) noexcept;
    /// A click on `button`: it takes the focus and is pressed.
    CardButton click(CardButton button) noexcept;

    [[nodiscard]] CardButton focused() const noexcept { return m_focused; }
    [[nodiscard]] bool shown() const noexcept { return m_shown; }
    [[nodiscard]] const std::string& gameId() const noexcept { return m_gameId; }

private:
    CardButton m_focused = CardButton::Play;
    bool m_shown = false;
    std::string m_gameId;
};

} // namespace rv::ui
