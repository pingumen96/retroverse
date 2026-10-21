#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The directory (I4 step 7a; ADR-0013 §10): the building's one panel, opened at the elevator and with M or Select in
// free roam, pad-navigable. Two tabs:
//  - "Piani": the plan's floors in the elevator's order (the walker's floor marked); a floor opens the list of its
//    rooms (the walker's room marked), whose first row is the floor's hall with the elevator. Choosing the hall rides to
//    the floor (ChooseFloor), choosing a room goes to its doorway (ChooseRoom), riding first when it is on another floor.
//  - "Preferiti e recenti": the catalog's favourites, then its most recently played games (catalog::RecentGame's
//    rule), each row a title and its system; a game opens its card, the quick catalog's card (GameCardContent, the
//    same readiness and blocker sentences) with Avvia (Launch) and "Mostra nella stanza" (ShowInRoom, only for a game
//    the plan places; else the card says why, quick_catalog_room.hpp's keys).
// Every sentence is the document's (assets/ui/directory.rml): the model holds data and keys. Floors are named by their
// building.json id (directory::kFloorIds), rooms by their kind and their sections' system names.
//
// Input (the ui_* actions through NavInput, as every product screen): Up and Down move in the list (Page Up and Down by
// directory::kPageRows, Home and End), Up past the first row reaches the tab bar, where Left and Right switch tabs and
// Down returns to the list; the shoulders switch tabs from anywhere but the card. Activate (A, Enter) or Right opens a
// floor's rooms; Activate chooses the hall or a room, opens a game's card, presses the card's focused button; Left and
// Right move between the card's buttons. Back leaves one level: the card to the games, the rooms to the floors (Left
// too), the floors or the games list closes the panel (Close). The commands that leave the panel (ChooseFloor,
// ChooseRoom, ShowInRoom, Close) close the model; Launch keeps it open on the card, which tells the outcome. A click on
// a tab, a row or a card button does what its key would (DirectoryClick).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/game_card_model.hpp>
#include <rv/ui/ui_command.hpp>

namespace rv::ui {

/// The ids, the data model and the keys the directory's document (assets/ui/directory.rml) and its screen
/// (rv::ui::DirectoryScreen) share (tests/unit/ui/test_directory.cpp checks the document against them).
namespace directory {
inline constexpr std::string_view kDocumentPath = "ui/directory.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/directory.rcss";
inline constexpr std::string_view kDataModel = "directory";
inline constexpr std::string_view kPanelId = "directory";
inline constexpr std::string_view kListId = "dir-list";
/// The tab bar's buttons.
inline constexpr std::string_view kFloorsTabId = "dir-tab-floors";
inline constexpr std::string_view kGamesTabId = "dir-tab-games";
/// Rows: "<prefix><index>" (the rooms list's row 0 is the floor's hall).
inline constexpr std::string_view kFloorRowPrefix = "dir-floor-";
inline constexpr std::string_view kRoomRowPrefix = "dir-room-";
inline constexpr std::string_view kGameRowPrefix = "dir-game-";
/// The card and its buttons, in focus order (DirectoryCardButton).
inline constexpr std::string_view kCardId = "dir-card";
inline constexpr std::string_view kCardPlayId = "dir-card-play";
inline constexpr std::string_view kCardRoomId = "dir-card-room";
inline constexpr std::string_view kCardBackId = "dir-card-back";
/// The data events of clicks: "tab('<floors|games>')", "row(<index>)" on a row of the list shown, "card('<play|room|
/// back>')".
inline constexpr std::string_view kTabEvent = "tab";
inline constexpr std::string_view kRowEvent = "row";
inline constexpr std::string_view kCardEvent = "card";
/// The floors the document names, building.json's ids in its order (rv_world's kGroundFloor first): another id shows
/// as itself.
inline constexpr std::array<std::string_view, 9> kFloorIds{"ground", "gen2", "gen3",     "gen4",    "gen5",
                                                           "gen6",   "gen7", "handheld", "computer"};
/// The room kinds (rv_world's RoomKind names, world::toString, which rv_ui does not link).
inline constexpr std::array<std::string_view, 3> kRoomKinds{"console", "shared", "arcade"};
/// The games list's states (DirectoryGamesState).
inline constexpr std::array<std::string_view, 3> kGamesStates{"loading", "ready", "failed"};
/// What the panel may tell under its list (setNotice): a ride or a teleport refused for now, the building not planned
/// yet. Empty: nothing.
inline constexpr std::array<std::string_view, 2> kNotices{"ride-unavailable", "building-pending"};
/// Rows a page (Page Up, Page Down, the triggers) moves.
inline constexpr std::size_t kPageRows = 8;
} // namespace directory

/// A system of a room's sections: its ES-DE name and the full name shown (the catalog's; empty: the name is shown).
struct DirectorySystem {
    std::string name;
    std::string fullName;

    bool operator==(const DirectorySystem&) const = default;
};

/// A room of a floor, as the app reads it from the plan (step 7b).
struct DirectoryRoom {
    /// The room's id ("room:snes", "area:shared:gen4", "area:arcade"), which ChooseRoom names.
    std::string id;
    /// One of directory::kRoomKinds.
    std::string kind;
    /// Its sections' systems, in the room's order (exactly one for a console room).
    std::vector<DirectorySystem> systems;
    /// The games it places.
    std::uint32_t games = 0;

    bool operator==(const DirectoryRoom&) const = default;
};

/// A floor of the plan.
struct DirectoryFloor {
    /// building.json's id, which ChooseFloor names.
    std::string id;
    /// Its place in the elevator's order (0 the ground floor): the elevator button the row shows.
    std::uint32_t order = 0;
    /// Its rooms, in the order the list shows them (the app's: along the hall from the elevator).
    std::vector<DirectoryRoom> rooms;

    bool operator==(const DirectoryFloor&) const = default;
};

/// What the "Piani" tab lists: the plan's floors in the elevator's order, and where the walker is.
struct DirectoryBuilding {
    std::vector<DirectoryFloor> floors;
    /// The floor the walker is on ("" none: the list starts on the first floor).
    std::string currentFloor;
    /// The room that holds the walker ("" in a hall).
    std::string currentRoom;

    bool operator==(const DirectoryBuilding&) const = default;
};

/// A calendar date as the games list shows it (the app gives the local date of catalog::RecentGame::lastPlayed).
struct DirectoryDate {
    int year = 0;
    int month = 0;
    int day = 0;

    bool operator==(const DirectoryDate&) const = default;
};

/// A game of the "Preferiti e recenti" tab (catalog::RecentGame as the app hands it over).
struct DirectoryGame {
    /// The catalog's id (GameId's text form), which Launch and ShowInRoom name.
    std::string gameId;
    std::string title;
    /// The system's ES-DE name and the full name shown (empty: the name is shown).
    std::string system;
    std::string systemName;
    bool favorite = false;
    /// When it was last played; nothing for a favourite never played.
    std::optional<DirectoryDate> played;

    bool operator==(const DirectoryGame&) const = default;
};

enum class DirectoryTab : std::uint8_t { Floors, Games };
/// "floors", "games" (the tab event's argument).
[[nodiscard]] std::string_view toString(DirectoryTab tab) noexcept;
[[nodiscard]] std::optional<DirectoryTab> directoryTabFromName(std::string_view name) noexcept;

/// What the panel shows.
enum class DirectoryView : std::uint8_t {
    Floors, ///< the floors (tab Piani)
    Rooms,  ///< a floor's hall and rooms (tab Piani)
    Games,  ///< the favourites and recent games
    Card,   ///< a game's card (tab Preferiti e recenti)
};
/// "floors", "rooms", "games", "card".
[[nodiscard]] std::string_view toString(DirectoryView view) noexcept;

/// The games list: asked for and not answered yet, answered, or the catalog could not be read.
enum class DirectoryGamesState : std::uint8_t { Loading, Ready, Failed };
/// "loading", "ready", "failed" (directory::kGamesStates).
[[nodiscard]] std::string_view toString(DirectoryGamesState state) noexcept;

/// The card's buttons, in focus order; ShowInRoom only for a game the plan places.
enum class DirectoryCardButton : std::uint8_t { Play, ShowInRoom, Back };
/// "play", "room", "back" (the card event's argument).
[[nodiscard]] std::string_view toString(DirectoryCardButton button) noexcept;
[[nodiscard]] std::optional<DirectoryCardButton> directoryCardButtonFromName(std::string_view name) noexcept;
/// The button's element id (directory::kCardPlayId ...).
[[nodiscard]] std::string_view elementIdOf(DirectoryCardButton button) noexcept;

/// What the directory asks of the app.
enum class DirectoryCommandKind : std::uint8_t {
    ChooseFloor, ///< ride to the floor `id` (its elevator)
    ChooseRoom,  ///< go to the doorway of room `id`, riding first when it is on another floor
    Launch,      ///< launch game `id` (the quick catalog's Avvia)
    ShowInRoom,  ///< show game `id` in its room (ADR-0012 §8, across floors: ADR-0013 §10)
    Close,       ///< the panel closed (Back)
};
/// "choose-floor", "choose-room", "launch", "show-in-room", "close".
[[nodiscard]] std::string_view toString(DirectoryCommandKind kind) noexcept;

struct DirectoryCommand {
    DirectoryCommandKind kind = DirectoryCommandKind::Close;
    /// The floor, room or game id; empty for Close.
    std::string id;

    bool operator==(const DirectoryCommand&) const = default;
};

/// A click on the panel (DirectoryScreen::takeClicked): a tab, a row of the list shown, a card button.
struct DirectoryClick {
    enum class Kind : std::uint8_t { Tab, Row, Card };
    Kind kind = Kind::Row;
    DirectoryTab tab = DirectoryTab::Floors;
    std::size_t row = 0;
    DirectoryCardButton button = DirectoryCardButton::Play;

    bool operator==(const DirectoryClick&) const = default;
};

/// The directory's state: the tab and view shown, the selections, the card, and what its input asks. Main thread; no
/// rendering. Every change of what the panel shows increments revision().
class DirectoryModel {
public:
    /// The floors and rooms (the plan's summary; step 7b fills it). The selections follow their floor and room by id;
    /// a rooms list whose floor is gone goes back to the floors.
    void setBuilding(DirectoryBuilding building);
    /// The games list (Ready): the selection follows its game by id, else stays at its index within the list.
    void setGames(std::vector<DirectoryGame> games);
    /// The list's state without a new list (Loading while the catalog worker answers, Failed).
    void setGamesState(DirectoryGamesState state);
    /// The card's content for the game it shows (ignored for another game: an answer that came late) and where the
    /// game stands: quick_catalog::kRoomPlaced (Mostra nella stanza shows), one of quick_catalog::kRoomUnplacedKeys
    /// (the card says why), empty while it is not known.
    void setCard(GameCardContent content, std::string room);
    /// A line under the list (directory::kNotices; empty: none), until the panel opens again.
    void setNotice(std::string key);

    /// Opens the panel on `tab`: the floors with the walker's floor selected, or the games with the first one selected
    /// (the list's own order: favourites, then the most recent). The list has the focus.
    void open(DirectoryTab tab);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return m_open; }

    /// A UI command (see the header). The command for the app, if any.
    std::optional<DirectoryCommand> apply(UiCommand command);
    /// A click: a tab switches to it, a row selects it and does what Activate does there, a card button presses it.
    std::optional<DirectoryCommand> click(const DirectoryClick& click);

    [[nodiscard]] DirectoryTab tab() const noexcept { return m_tab; }
    [[nodiscard]] DirectoryView view() const noexcept { return m_view; }
    /// The tab bar has the focus (else the list, or the card's buttons in the card): the user moved it there, or the
    /// list shown has no row (until rows come).
    [[nodiscard]] bool tabsFocused() const noexcept { return m_tabsFocused; }
    /// The element the focus is on: a tab, a row, a card button (directory's ids).
    [[nodiscard]] std::string focusedElement() const;

    [[nodiscard]] const DirectoryBuilding& building() const noexcept { return m_building; }
    /// The selected floor's index (0 without floors).
    [[nodiscard]] std::size_t selectedFloor() const noexcept { return m_floor; }
    /// The floor whose rooms the Rooms view lists; null outside it.
    [[nodiscard]] const DirectoryFloor* openedFloor() const noexcept;
    /// The rooms list's selected row: 0 the hall, i the room i - 1.
    [[nodiscard]] std::size_t selectedRoomRow() const noexcept { return m_roomRow; }
    /// The rows of the list the view shows (floors, the hall and rooms, games; 0 in the card).
    [[nodiscard]] std::size_t rowCount() const noexcept;
    /// The selected row of the list the view shows.
    [[nodiscard]] std::size_t selectedRow() const noexcept;

    [[nodiscard]] const std::vector<DirectoryGame>& games() const noexcept { return m_games; }
    [[nodiscard]] DirectoryGamesState gamesState() const noexcept { return m_gamesState; }
    [[nodiscard]] std::size_t selectedGame() const noexcept { return m_game; }

    /// The game the card shows ("" outside the card), its content and where it stands (setCard).
    [[nodiscard]] const std::string& cardGameId() const noexcept { return m_cardGameId; }
    [[nodiscard]] const GameCardContent& cardContent() const noexcept { return m_cardContent; }
    [[nodiscard]] const std::string& cardRoom() const noexcept { return m_cardRoom; }
    [[nodiscard]] DirectoryCardButton cardButton() const noexcept { return m_cardButton; }
    /// Mostra nella stanza shows: the card's game is placed.
    [[nodiscard]] bool cardShowsRoom() const noexcept;

    [[nodiscard]] const std::string& notice() const noexcept { return m_notice; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void changed() noexcept { ++m_revision; }
    /// Moves the selection of the list shown by `delta` rows, stopping at its ends; Up past the first row focuses the
    /// tab bar.
    void move(std::ptrdiff_t delta, bool toTabs);
    void selectRow(std::size_t row);
    void switchTab(DirectoryTab tab);
    void openRooms();
    void openCard();
    /// Leaves one level; the Close command when the panel closes.
    std::optional<DirectoryCommand> back();
    /// What Activate does on the list's selected row.
    std::optional<DirectoryCommand> activateRow();
    std::optional<DirectoryCommand> press(DirectoryCardButton button);
    std::optional<DirectoryCommand> leave(DirectoryCommandKind kind, std::string id);
    /// The list shown has no row: the tab bar takes the focus; once rows come, the list takes it back (unless the
    /// user moved it since).
    void followRows() noexcept;
    [[nodiscard]] std::size_t floorIndexOf(std::string_view id) const noexcept;
    /// The row the rooms list of floor `floor` starts on: the walker's room there, else the first room, else the hall.
    [[nodiscard]] std::size_t startRoomRow(std::size_t floor) const noexcept;

    bool m_open = false;
    DirectoryTab m_tab = DirectoryTab::Floors;
    DirectoryView m_view = DirectoryView::Floors;
    /// The view the Piani tab shows when it comes back (Floors or Rooms).
    DirectoryView m_floorsView = DirectoryView::Floors;
    bool m_tabsFocused = false;
    /// The tab bar has the focus because the list had no row.
    bool m_tabsForEmpty = false;

    DirectoryBuilding m_building;
    std::size_t m_floor = 0;
    std::size_t m_openedFloor = 0;
    std::size_t m_roomRow = 0;

    std::vector<DirectoryGame> m_games;
    DirectoryGamesState m_gamesState = DirectoryGamesState::Loading;
    std::size_t m_game = 0;

    std::string m_cardGameId;
    GameCardContent m_cardContent;
    std::string m_cardRoom;
    DirectoryCardButton m_cardButton = DirectoryCardButton::Play;

    std::string m_notice;
    std::uint64_t m_revision = 0;
};

} // namespace rv::ui
