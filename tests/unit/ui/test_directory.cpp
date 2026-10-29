// I4 step 7a: the directory (ADR-0013 §10): its model's transitions (the floors, a floor's hall and rooms, the
// favourites and recent games, a game's card; the tab bar; Back's levels; the commands it emits and the panel closing
// with those that leave it; clicks; the selections following new data by id; the focus on an empty list) and the
// contract between its document (assets/ui/directory.rml and .rcss) and its screen (rv::ui::DirectoryScreen, whose
// GPU side the unit tests cannot load: they share rv/ui/directory_model.hpp): the ids, the data model and its
// bindings, every floor id, room kind, games state and notice the document names, the card's readiness, blocker and
// room sentences equal to the quick catalog's (but for a game the plan does not hold, which the building words its own
// way) and its outcome sentences equal to the world card's, line for line; and
// tests/input_scripts/directory_panel.json played GPU-free through the action map and NavInput over rv_ui_probe's
// model (tests/ui_probe/directory_fixture.hpp), as tests/smoke/test_directory_panel.cpp plays it on the GPU.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/ui/directory_model.hpp>
#include <rv/ui/game_card_model.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/quick_catalog_room.hpp>
#include <rv/ui/ui_assets.hpp>

#include "../../ui_probe/directory_fixture.hpp"

namespace dir = rv::ui::directory;
namespace qc = rv::ui::quick_catalog;
using rv::ui::DirectoryAction;
using rv::ui::DirectoryBuilding;
using rv::ui::DirectoryCardButton;
using rv::ui::DirectoryClick;
using rv::ui::DirectoryCommand;
using rv::ui::DirectoryCommandKind;
using rv::ui::DirectoryFloor;
using rv::ui::DirectoryGame;
using rv::ui::DirectoryGamesState;
using rv::ui::DirectoryModel;
using rv::ui::DirectoryRoom;
using rv::ui::DirectoryTab;
using rv::ui::DirectoryView;
using rv::ui::GameCardContent;
using rv::ui::UiCommand;

namespace {

DirectoryRoom room(std::string id, std::string kind, std::vector<std::string> systems, std::uint32_t games) {
    DirectoryRoom result{std::move(id), std::move(kind), {}, games};
    for (std::string& system : systems) {
        result.systems.push_back({system, system + " full"});
    }
    return result;
}

/// Three floors: the ground floor's arcade room, gen4's two rooms and the walker in room:snes, handheld without rooms.
DirectoryBuilding building() {
    DirectoryBuilding result;
    result.floors.push_back({"ground", 0, {room("area:arcade", "arcade", {"mame", "fbneo"}, 15)}});
    result.floors.push_back({"gen4",
                             3,
                             {room("room:megadrive", "console", {"megadrive"}, 40),
                              room("room:snes", "console", {"snes"}, 50),
                              room("area:shared:gen4", "shared", {"neogeo", "segacd"}, 3)}});
    result.floors.push_back({"handheld", 7, {}});
    result.currentFloor = "gen4";
    result.currentRoom = "room:snes";
    return result;
}

std::vector<DirectoryGame> games(std::size_t count) {
    std::vector<DirectoryGame> result;
    for (std::size_t index = 0; index < count; ++index) {
        DirectoryGame game;
        game.gameId = "g000000000000000" + std::to_string(index + 1);
        game.title = "Game " + std::to_string(index + 1);
        game.system = "snes";
        game.systemName = "Lumen 16 Home System";
        game.favorite = index < 2;
        result.push_back(game);
    }
    return result;
}

DirectoryCommand command(DirectoryCommandKind kind, std::string id = {}) {
    return DirectoryCommand{kind, std::move(id)};
}

/// The model opened on the floors of building().
DirectoryModel floorsModel() {
    DirectoryModel model;
    model.setBuilding(building());
    model.open(DirectoryTab::Floors);
    return model;
}

/// The model on the card of the first game of games(5), placed in the plan or not.
DirectoryModel cardModel(std::string room) {
    DirectoryModel model;
    model.setGames(games(5));
    model.open(DirectoryTab::Games);
    REQUIRE_FALSE(model.apply(UiCommand::Activate).has_value());
    GameCardContent content = model.cardContent();
    content.details = true;
    content.readiness = "ready";
    model.setCard(content, std::move(room));
    return model;
}

std::string readAsset(std::string_view relative) {
    const std::filesystem::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto text = rv::core::readTextFile(assets / rv::core::pathFromUtf8(relative));
    REQUIRE(text.has_value());
    return *text;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

/// The sentences of a document's paragraphs whose condition names `key`, by their condition (test_game_card.cpp's).
std::map<std::string, std::string> sentencesOf(const std::string& rml, std::string_view key) {
    std::map<std::string, std::string> sentences;
    static const std::regex paragraph(R"re(<p [^>]*data-if="([^"]*)"[^>]*>([^<]*)</p>)re");
    for (auto it = std::sregex_iterator(rml.begin(), rml.end(), paragraph); it != std::sregex_iterator(); ++it) {
        const std::string condition = (*it)[1].str();
        if (condition.find(key) != std::string::npos) {
            sentences.emplace(condition, (*it)[2].str());
        }
    }
    return sentences;
}

} // namespace

TEST_CASE("ui/directory: names, element ids and the card's buttons", "[ui][directory]") {
    CHECK(rv::ui::toString(DirectoryTab::Floors) == "floors");
    CHECK(rv::ui::toString(DirectoryTab::Games) == "games");
    CHECK(rv::ui::directoryTabFromName("games") == DirectoryTab::Games);
    CHECK_FALSE(rv::ui::directoryTabFromName("Games").has_value());
    CHECK(rv::ui::toString(DirectoryView::Rooms) == "rooms");
    CHECK(rv::ui::toString(DirectoryView::Card) == "card");
    CHECK(rv::ui::toString(DirectoryGamesState::Loading) == dir::kGamesStates[0]);
    CHECK(rv::ui::toString(DirectoryGamesState::Failed) == dir::kGamesStates[2]);
    for (const DirectoryCardButton button :
         {DirectoryCardButton::Play, DirectoryCardButton::ShowInRoom, DirectoryCardButton::Back}) {
        CHECK(rv::ui::directoryCardButtonFromName(rv::ui::toString(button)) == button);
    }
    CHECK(rv::ui::elementIdOf(DirectoryCardButton::ShowInRoom) == dir::kCardRoomId);
    CHECK(rv::ui::toString(DirectoryCommandKind::ChooseFloor) == "choose-floor");
    CHECK(rv::ui::toString(DirectoryCommandKind::ShowInRoom) == "show-in-room");
}

TEST_CASE("ui/directory: the floors, the walker's floor selected; the tab bar above the list", "[ui][directory]") {
    DirectoryModel model;
    CHECK_FALSE(model.isOpen());
    // Closed, it takes no command.
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    model.setBuilding(building());
    model.open(DirectoryTab::Floors);
    CHECK(model.isOpen());
    CHECK(model.tab() == DirectoryTab::Floors);
    CHECK(model.view() == DirectoryView::Floors);
    CHECK(model.selectedFloor() == 1);
    CHECK(model.rowCount() == 3);
    CHECK(model.focusedElement() == "dir-floor-1");
    CHECK_FALSE(model.tabsFocused());

    // Up and down within the list, stopping at its end; Home, End, pages.
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.selectedFloor() == 2);
    model.apply(UiCommand::Down);
    CHECK(model.selectedFloor() == 2);
    model.apply(UiCommand::Home);
    CHECK(model.selectedFloor() == 0);
    model.apply(UiCommand::End);
    CHECK(model.selectedFloor() == 2);
    model.apply(UiCommand::PageUp);
    CHECK(model.selectedFloor() == 0);
    model.apply(UiCommand::PageDown);
    CHECK(model.selectedFloor() == 2);
    model.apply(UiCommand::Home);
    // Up past the first row: the tab bar, where Left and Right switch tabs and Down returns to the list.
    model.apply(UiCommand::Up);
    CHECK(model.tabsFocused());
    CHECK(model.focusedElement() == dir::kFloorsTabId);
    model.apply(UiCommand::Up);
    CHECK(model.tabsFocused());
    model.apply(UiCommand::Right);
    CHECK(model.tab() == DirectoryTab::Games);
    CHECK(model.view() == DirectoryView::Games);
    CHECK(model.focusedElement() == dir::kGamesTabId);
    model.apply(UiCommand::Left);
    CHECK(model.tab() == DirectoryTab::Floors);
    CHECK(model.tabsFocused());
    model.apply(UiCommand::Down);
    CHECK_FALSE(model.tabsFocused());
    CHECK(model.focusedElement() == "dir-floor-0");
    // Other commands do nothing on the floors.
    const std::uint64_t revision = model.revision();
    for (const UiCommand ignored : {UiCommand::Left, UiCommand::Media, UiCommand::Info}) {
        CHECK_FALSE(model.apply(ignored).has_value());
    }
    CHECK(model.revision() == revision);
}

TEST_CASE("ui/directory: a floor opens its hall and rooms; choosing the hall rides, a room goes to it",
          "[ui][directory]") {
    SECTION("the walker's floor starts on the walker's room; Activate on it chooses the room") {
        DirectoryModel model = floorsModel();
        CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
        CHECK(model.view() == DirectoryView::Rooms);
        REQUIRE(model.openedFloor() != nullptr);
        CHECK(model.openedFloor()->id == "gen4");
        CHECK(model.rowCount() == 4);
        CHECK(model.selectedRoomRow() == 2);
        CHECK(model.focusedElement() == "dir-room-2");
        CHECK(model.apply(UiCommand::Activate) == command(DirectoryCommandKind::ChooseRoom, "room:snes"));
        CHECK_FALSE(model.isOpen());
    }
    SECTION("another floor starts on its first room; the hall row chooses the floor") {
        DirectoryModel model = floorsModel();
        model.apply(UiCommand::Up);
        CHECK(model.selectedFloor() == 0);
        // Right opens the rooms as Activate does.
        CHECK_FALSE(model.apply(UiCommand::Right).has_value());
        CHECK(model.view() == DirectoryView::Rooms);
        CHECK(model.selectedRoomRow() == 1);
        model.apply(UiCommand::Up);
        CHECK(model.selectedRoomRow() == 0);
        CHECK(model.focusedElement() == "dir-room-0");
        CHECK(model.apply(UiCommand::Activate) == command(DirectoryCommandKind::ChooseFloor, "ground"));
        CHECK_FALSE(model.isOpen());
    }
    SECTION("a floor without rooms starts on its hall") {
        DirectoryModel model = floorsModel();
        model.apply(UiCommand::Down);
        model.apply(UiCommand::Activate);
        CHECK(model.rowCount() == 1);
        CHECK(model.selectedRoomRow() == 0);
        CHECK(model.apply(UiCommand::Activate) == command(DirectoryCommandKind::ChooseFloor, "handheld"));
    }
    SECTION("Left and Back go back to the floors, the opened floor selected; Back there closes the panel") {
        DirectoryModel model = floorsModel();
        model.apply(UiCommand::Up);
        model.apply(UiCommand::Activate);
        model.apply(UiCommand::Down);
        CHECK_FALSE(model.apply(UiCommand::Left).has_value());
        CHECK(model.view() == DirectoryView::Floors);
        CHECK(model.selectedFloor() == 0);
        model.apply(UiCommand::Right);
        CHECK_FALSE(model.apply(UiCommand::Back).has_value());
        CHECK(model.view() == DirectoryView::Floors);
        CHECK(model.isOpen());
        CHECK(model.apply(UiCommand::Back) == command(DirectoryCommandKind::Close));
        CHECK_FALSE(model.isOpen());
    }
    SECTION("the rooms' list reaches the tab bar too, and Back there leaves the rooms") {
        DirectoryModel model = floorsModel();
        model.apply(UiCommand::Activate);
        model.apply(UiCommand::Home);
        model.apply(UiCommand::Up);
        CHECK(model.tabsFocused());
        CHECK(model.view() == DirectoryView::Rooms);
        CHECK_FALSE(model.apply(UiCommand::Back).has_value());
        CHECK(model.view() == DirectoryView::Floors);
        CHECK_FALSE(model.tabsFocused());
    }
}

TEST_CASE("ui/directory: the shoulders switch tabs; the floors' tab comes back where it was", "[ui][directory]") {
    DirectoryModel model = floorsModel();
    model.setGames(games(3));
    model.apply(UiCommand::Activate);
    CHECK(model.view() == DirectoryView::Rooms);
    model.apply(UiCommand::FilterNext);
    CHECK(model.tab() == DirectoryTab::Games);
    CHECK(model.view() == DirectoryView::Games);
    CHECK_FALSE(model.tabsFocused());
    CHECK(model.focusedElement() == "dir-game-0");
    model.apply(UiCommand::FilterPrevious);
    CHECK(model.view() == DirectoryView::Rooms);
    CHECK(model.selectedRoomRow() == 2);
}

TEST_CASE("ui/directory: the games list, its loading and failure, and the focus on an empty list", "[ui][directory]") {
    DirectoryModel model;
    model.setBuilding(building());
    model.open(DirectoryTab::Games);
    CHECK(model.view() == DirectoryView::Games);
    CHECK(model.gamesState() == DirectoryGamesState::Loading);
    // No row: the tab bar holds the focus, and the list takes it back once the games come.
    CHECK(model.tabsFocused());
    CHECK(model.focusedElement() == dir::kGamesTabId);
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.tabsFocused());
    model.setGames(games(12));
    CHECK(model.gamesState() == DirectoryGamesState::Ready);
    CHECK_FALSE(model.tabsFocused());
    CHECK(model.focusedElement() == "dir-game-0");
    model.apply(UiCommand::PageDown);
    CHECK(model.selectedGame() == dir::kPageRows);
    model.apply(UiCommand::End);
    CHECK(model.selectedGame() == 11);
    // A new list keeps the selection on its game, else at its index within the list.
    std::vector<DirectoryGame> shifted = games(12);
    shifted.insert(shifted.begin(), games(13).back());
    model.setGames(shifted);
    CHECK(model.selectedGame() == 12);
    CHECK(model.games()[model.selectedGame()].gameId == games(12).back().gameId);
    model.setGames(games(4));
    CHECK(model.selectedGame() == 3);
    // A failure shows no list: the tab bar takes the focus.
    model.setGamesState(DirectoryGamesState::Failed);
    CHECK(model.rowCount() == 0);
    CHECK(model.tabsFocused());
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == DirectoryView::Games);
    // Back closes the panel from the games, also from the tab bar.
    CHECK(model.apply(UiCommand::Back) == command(DirectoryCommandKind::Close));
    // Opened again on the games: the first one.
    model.setGames(games(4));
    model.open(DirectoryTab::Games);
    CHECK(model.selectedGame() == 0);
    CHECK_FALSE(model.tabsFocused());
}

TEST_CASE("ui/directory: a game's card: Avvia launches, Mostra nella stanza shows a placed game, Back returns",
          "[ui][directory]") {
    DirectoryModel model;
    model.setGames(games(5));
    model.open(DirectoryTab::Games);
    model.apply(UiCommand::Down);
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == DirectoryView::Card);
    CHECK(model.cardGameId() == games(5)[1].gameId);
    // The card starts from the row's title and system, its details coming from the app.
    CHECK(model.cardContent().gameId == games(5)[1].gameId);
    CHECK(model.cardContent().title == "Game 2");
    CHECK(model.cardContent().platform == "Lumen 16 Home System");
    CHECK_FALSE(model.cardContent().details);
    CHECK(model.cardRoom().empty());
    CHECK(model.cardButton() == DirectoryCardButton::Play);
    CHECK(model.focusedElement() == dir::kCardPlayId);
    // Without a placement known, Mostra nella stanza is not there: Right goes to Indietro.
    model.apply(UiCommand::Right);
    CHECK(model.cardButton() == DirectoryCardButton::Back);
    model.apply(UiCommand::Right);
    CHECK(model.cardButton() == DirectoryCardButton::Back);
    model.apply(UiCommand::Left);
    CHECK(model.cardButton() == DirectoryCardButton::Play);
    // An answer for another game is ignored; this game's fills the card.
    GameCardContent other;
    other.gameId = games(5)[0].gameId;
    model.setCard(other, std::string(qc::kRoomPlaced));
    CHECK(model.cardRoom().empty());
    GameCardContent content = model.cardContent();
    content.details = true;
    content.readiness = "ready";
    model.setCard(content, std::string(qc::kRoomPlaced));
    CHECK(model.cardContent().details);
    CHECK(model.cardShowsRoom());
    model.apply(UiCommand::Down);
    CHECK(model.cardButton() == DirectoryCardButton::ShowInRoom);
    CHECK(model.focusedElement() == dir::kCardRoomId);
    // Avvia launches and keeps the card (it tells the outcome).
    model.apply(UiCommand::Up);
    CHECK(model.apply(UiCommand::Activate) == command(DirectoryCommandKind::Launch, games(5)[1].gameId));
    CHECK(model.isOpen());
    CHECK(model.view() == DirectoryView::Card);
    // Other commands do nothing on the card, the shoulders included.
    for (const UiCommand ignored : {UiCommand::Media, UiCommand::Info, UiCommand::FilterNext, UiCommand::PageDown}) {
        CHECK_FALSE(model.apply(ignored).has_value());
    }
    CHECK(model.view() == DirectoryView::Card);
    // Back returns to the games, the game still selected; Indietro likewise.
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.view() == DirectoryView::Games);
    CHECK(model.cardGameId().empty());
    CHECK(model.focusedElement() == "dir-game-1");
    model.apply(UiCommand::Activate);
    model.apply(UiCommand::Right);
    model.apply(UiCommand::Right);
    CHECK(model.cardButton() == DirectoryCardButton::Back);
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == DirectoryView::Games);
}

TEST_CASE("ui/directory: openGame() opens straight at a game's card, the games list behind it (I6 step 5)",
          "[ui][directory]") {
    DirectoryModel model;
    const DirectoryGame game = games(5)[3];
    const std::uint64_t gamesAsked = model.gamesRequest();
    const std::uint64_t cardsAsked = model.cardRequest();
    model.openGame(game);
    CHECK(model.isOpen());
    CHECK(model.tab() == DirectoryTab::Games);
    CHECK(model.view() == DirectoryView::Card);
    CHECK(model.cardGameId() == game.gameId);
    CHECK(model.cardContent().title == game.title);
    CHECK(model.cardButton() == DirectoryCardButton::Play);
    CHECK(model.focusedElement() == dir::kCardPlayId);
    CHECK(model.gamesRequest() == gamesAsked + 1); // the list asked for behind the card
    CHECK(model.cardRequest() == cardsAsked + 1);  // and the card's content
    // The list comes: the card stays; Back leads to the list with the game selected.
    model.setGames(games(5));
    CHECK(model.view() == DirectoryView::Card);
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.view() == DirectoryView::Games);
    CHECK(model.focusedElement() == "dir-game-3");
    // Avvia on the card launches the game.
    model.openGame(game);
    CHECK(model.apply(UiCommand::Activate) == command(DirectoryCommandKind::Launch, game.gameId));
}

TEST_CASE("ui/directory: Mostra nella stanza leaves the panel; an unplaced game has none", "[ui][directory]") {
    SECTION("placed") {
        DirectoryModel model = cardModel(std::string(qc::kRoomPlaced));
        model.apply(UiCommand::Right);
        CHECK(model.cardButton() == DirectoryCardButton::ShowInRoom);
        CHECK(model.apply(UiCommand::Activate) == command(DirectoryCommandKind::ShowInRoom, games(5)[0].gameId));
        CHECK_FALSE(model.isOpen());
    }
    SECTION("not placed: the button is skipped and a click on it does nothing") {
        DirectoryModel model = cardModel("no-cover");
        CHECK_FALSE(model.cardShowsRoom());
        model.apply(UiCommand::Right);
        CHECK(model.cardButton() == DirectoryCardButton::Back);
        CHECK_FALSE(model.click({DirectoryClick::Kind::Card, DirectoryTab::Floors, 0, DirectoryCardButton::ShowInRoom})
                        .has_value());
        CHECK(model.isOpen());
    }
    SECTION("a placement lost while Mostra nella stanza has the focus moves it to Avvia") {
        DirectoryModel model = cardModel(std::string(qc::kRoomPlaced));
        model.apply(UiCommand::Right);
        GameCardContent content = model.cardContent();
        model.setCard(content, "room-kept");
        CHECK(model.cardButton() == DirectoryCardButton::Play);
    }
}

TEST_CASE("ui/directory: clicks on tabs, rows and the card's buttons", "[ui][directory]") {
    DirectoryModel model = floorsModel();
    model.setGames(games(3));
    // A floor's row opens its rooms; a room's row chooses it.
    CHECK_FALSE(model.click({DirectoryClick::Kind::Row, DirectoryTab::Floors, 0, DirectoryCardButton::Play})
                    .has_value());
    CHECK(model.view() == DirectoryView::Rooms);
    CHECK(model.openedFloor()->id == "ground");
    // A row out of the list does nothing.
    CHECK_FALSE(model.click({DirectoryClick::Kind::Row, DirectoryTab::Floors, 9, DirectoryCardButton::Play})
                    .has_value());
    CHECK(model.click({DirectoryClick::Kind::Row, DirectoryTab::Floors, 1, DirectoryCardButton::Play}) ==
          command(DirectoryCommandKind::ChooseRoom, "area:arcade"));
    CHECK_FALSE(model.isOpen());
    CHECK_FALSE(model.click({DirectoryClick::Kind::Tab, DirectoryTab::Games, 0, DirectoryCardButton::Play})
                    .has_value());

    // A tab's click switches to it with the list focused.
    model.open(DirectoryTab::Floors);
    model.apply(UiCommand::Up);
    model.apply(UiCommand::Up);
    CHECK(model.tabsFocused());
    model.click({DirectoryClick::Kind::Tab, DirectoryTab::Games, 0, DirectoryCardButton::Play});
    CHECK(model.tab() == DirectoryTab::Games);
    CHECK_FALSE(model.tabsFocused());
    // A game's row opens its card; another game's row beside the card opens that one's.
    model.click({DirectoryClick::Kind::Row, DirectoryTab::Floors, 2, DirectoryCardButton::Play});
    CHECK(model.cardGameId() == games(3)[2].gameId);
    model.click({DirectoryClick::Kind::Row, DirectoryTab::Floors, 0, DirectoryCardButton::Play});
    CHECK(model.view() == DirectoryView::Card);
    CHECK(model.cardGameId() == games(3)[0].gameId);
    // The card's buttons: Avvia launches, Indietro returns.
    CHECK(model.click({DirectoryClick::Kind::Card, DirectoryTab::Floors, 0, DirectoryCardButton::Play}) ==
          command(DirectoryCommandKind::Launch, games(3)[0].gameId));
    CHECK_FALSE(model.click({DirectoryClick::Kind::Card, DirectoryTab::Floors, 0, DirectoryCardButton::Back})
                    .has_value());
    CHECK(model.view() == DirectoryView::Games);
    // A card button with no card does nothing; the floors' tab leaves a card.
    CHECK_FALSE(model.click({DirectoryClick::Kind::Card, DirectoryTab::Floors, 0, DirectoryCardButton::Play})
                    .has_value());
    model.apply(UiCommand::Activate);
    model.click({DirectoryClick::Kind::Tab, DirectoryTab::Floors, 0, DirectoryCardButton::Play});
    CHECK(model.view() == DirectoryView::Floors);
    CHECK(model.cardGameId().empty());
}

TEST_CASE("ui/directory: a new building keeps the selections by id; a floor gone goes back to the floors",
          "[ui][directory]") {
    DirectoryModel model = floorsModel();
    model.apply(UiCommand::Activate);
    CHECK(model.selectedRoomRow() == 2);
    // A room enters before room:snes and a floor before gen4: both selections follow their ids.
    DirectoryBuilding grown = building();
    grown.floors.insert(grown.floors.begin() + 1, DirectoryFloor{"gen3", 2, {room("room:nes", "console", {"nes"}, 9)}});
    grown.floors[2].rooms.insert(grown.floors[2].rooms.begin(), room("room:tg16", "console", {"tg16"}, 31));
    model.setBuilding(grown);
    CHECK(model.openedFloor()->id == "gen4");
    CHECK(model.selectedRoomRow() == 3);
    CHECK(model.openedFloor()->rooms[2].id == "room:snes");
    model.apply(UiCommand::Back);
    CHECK(model.selectedFloor() == 2);
    // The floor whose rooms are listed is gone: the floors.
    model.apply(UiCommand::Activate);
    DirectoryBuilding shrunk = building();
    shrunk.floors.erase(shrunk.floors.begin() + 1);
    model.setBuilding(shrunk);
    CHECK(model.view() == DirectoryView::Floors);
    CHECK(model.openedFloor() == nullptr);
    // No floor at all (no plan yet): the tab bar has the focus until the building comes.
    model.setBuilding(DirectoryBuilding{});
    CHECK(model.rowCount() == 0);
    CHECK(model.tabsFocused());
    model.setBuilding(building());
    CHECK_FALSE(model.tabsFocused());
    CHECK(model.focusedElement() == "dir-floor-0");
}

TEST_CASE("ui/directory: the notice lasts until the panel opens again; every change counts a revision",
          "[ui][directory]") {
    DirectoryModel model = floorsModel();
    const std::uint64_t opened = model.revision();
    model.setNotice(std::string(dir::kNotices[0]));
    CHECK(model.notice() == dir::kNotices[0]);
    CHECK(model.revision() > opened);
    const std::uint64_t noticed = model.revision();
    model.setNotice(std::string(dir::kNotices[0]));
    CHECK(model.revision() == noticed);
    model.apply(UiCommand::Down);
    CHECK(model.revision() > noticed);
    model.close();
    CHECK_FALSE(model.isOpen());
    model.open(DirectoryTab::Floors);
    CHECK(model.notice().empty());
}

TEST_CASE("ui/directory: a click on the shown card's own row keeps the card; a card opened again asks again",
          "[ui][directory]") {
    SECTION("the model") {
        DirectoryModel model;
        model.setGames(games(3));
        model.open(DirectoryTab::Games);
        const std::uint64_t requests = model.cardRequest();
        model.apply(UiCommand::Activate);
        CHECK(model.cardRequest() == requests + 1);
        GameCardContent content = model.cardContent();
        content.details = true;
        content.readiness = "ready";
        model.setCard(content, std::string(qc::kRoomPlaced));
        model.apply(UiCommand::Right);
        CHECK(model.cardButton() == DirectoryCardButton::ShowInRoom);
        // The chosen row beside the card is the card's game: nothing changes, nothing is asked.
        const std::uint64_t revision = model.revision();
        CHECK_FALSE(model.click({DirectoryClick::Kind::Row, DirectoryTab::Floors, 0, DirectoryCardButton::Play})
                        .has_value());
        CHECK(model.view() == DirectoryView::Card);
        CHECK(model.cardGameId() == games(3)[0].gameId);
        CHECK(model.cardContent().details);
        CHECK(model.cardShowsRoom());
        CHECK(model.cardButton() == DirectoryCardButton::ShowInRoom);
        CHECK(model.cardRequest() == requests + 1);
        CHECK(model.revision() == revision);
        // Back and Activate in one frame: the same game's card again, from the row, asked for again.
        model.apply(UiCommand::Back);
        model.apply(UiCommand::Activate);
        CHECK(model.cardGameId() == games(3)[0].gameId);
        CHECK_FALSE(model.cardContent().details);
        CHECK(model.cardRoom().empty());
        CHECK(model.cardRequest() == requests + 2);
        // Another game's row: its card, asked for.
        model.click({DirectoryClick::Kind::Row, DirectoryTab::Floors, 1, DirectoryCardButton::Play});
        CHECK(model.cardGameId() == games(3)[1].gameId);
        CHECK(model.cardRequest() == requests + 3);
    }
    SECTION("the app's side (directory_fixture's fillCard): the card filled again") {
        namespace fixture = rv::probe::directory_fixture;
        DirectoryModel model;
        model.setGames(fixture::games());
        model.open(DirectoryTab::Games);
        std::uint64_t served = 0;
        model.apply(UiCommand::Activate);
        fixture::fillCard(model, served);
        CHECK(model.cardContent().details);
        model.apply(UiCommand::Back);
        model.apply(UiCommand::Activate);
        fixture::fillCard(model, served);
        CHECK(model.cardContent().details);
        CHECK(model.cardRoom() == qc::kRoomPlaced);
    }
}

TEST_CASE("ui/directory: the games list is asked for on entering it, and opens on its first game", "[ui][directory]") {
    // A list read when the panel was last open: [1, 2, 3, 4].
    DirectoryModel model;
    model.setBuilding(building());
    model.setGames(games(4));
    std::uint64_t requests = model.gamesRequest();
    // Opened on the games: a request; the stale list shows meanwhile, its first game selected.
    model.open(DirectoryTab::Games);
    CHECK(model.gamesRequest() == ++requests);
    model.setGamesState(DirectoryGamesState::Loading);
    CHECK(model.selectedGame() == 0);
    // The answer: game 5 played since, first. The selection stays on the first row, not on game 1.
    std::vector<DirectoryGame> fresh = games(5);
    fresh.insert(fresh.begin(), fresh.back());
    fresh.pop_back();
    model.setGames(fresh);
    CHECK(model.selectedGame() == 0);
    CHECK(model.games()[0].gameId == games(5)[4].gameId);
    CHECK(model.focusedElement() == "dir-game-0");
    // Once the user moved, a new list follows the selected game.
    model.apply(UiCommand::Down);
    const std::string selected = model.games()[model.selectedGame()].gameId;
    model.setGames(games(5));
    CHECK(model.games()[model.selectedGame()].gameId == selected);
    // A card follows its game too (the card's row stays chosen).
    model.apply(UiCommand::Home);
    model.apply(UiCommand::Activate);
    model.setGames(fresh);
    CHECK(model.games()[model.selectedGame()].gameId == model.cardGameId());
    // Back to the list: no request; the floors and the games again: a request each time the games are entered.
    model.apply(UiCommand::Back);
    CHECK(model.gamesRequest() == requests);
    model.apply(UiCommand::FilterPrevious);
    CHECK(model.gamesRequest() == requests);
    model.apply(UiCommand::FilterNext);
    CHECK(model.gamesRequest() == ++requests);
    model.apply(UiCommand::Home);
    model.apply(UiCommand::Up);
    model.apply(UiCommand::Left);
    model.apply(UiCommand::Right);
    CHECK(model.gamesRequest() == ++requests);
    model.click({DirectoryClick::Kind::Tab, DirectoryTab::Floors, 0, DirectoryCardButton::Play});
    model.click({DirectoryClick::Kind::Tab, DirectoryTab::Games, 0, DirectoryCardButton::Play});
    CHECK(model.gamesRequest() == ++requests);
    // Opened on the floors: no request, but the games start from their first one when the tab is entered.
    model.apply(UiCommand::Down);
    CHECK(model.selectedGame() == 1);
    model.close();
    model.open(DirectoryTab::Floors);
    CHECK(model.gamesRequest() == requests);
    CHECK(model.selectedFloor() == 1);
    model.apply(UiCommand::FilterNext);
    CHECK(model.gamesRequest() == ++requests);
    CHECK(model.selectedGame() == 0);
    model.setGames(games(5));
    CHECK(model.selectedGame() == 0);
}

TEST_CASE("ui/directory: reopen() shows the panel where a refused command left it", "[ui][directory]") {
    SECTION("a room chosen: its floor's rooms, the room selected") {
        DirectoryModel model = floorsModel();
        model.apply(UiCommand::Activate);
        model.apply(UiCommand::Down);
        CHECK(model.apply(UiCommand::Activate) == command(DirectoryCommandKind::ChooseRoom, "area:shared:gen4"));
        CHECK_FALSE(model.isOpen());
        model.reopen();
        CHECK(model.isOpen());
        CHECK(model.view() == DirectoryView::Rooms);
        REQUIRE(model.openedFloor() != nullptr);
        CHECK(model.openedFloor()->id == "gen4");
        CHECK(model.focusedElement() == "dir-room-3");
        CHECK(model.notice().empty());
        model.setNotice(std::string(dir::kNotices[0]));
        CHECK(model.notice() == dir::kNotices[0]);
        // Open, reopen() does nothing.
        const std::uint64_t revision = model.revision();
        model.reopen();
        CHECK(model.revision() == revision);
        // open() starts again from the walker's floor.
        model.close();
        model.open(DirectoryTab::Floors);
        CHECK(model.view() == DirectoryView::Floors);
        CHECK(model.focusedElement() == "dir-floor-1");
    }
    SECTION("the walker's own floor's hall: ChooseFloor names the walker's floor; reopen() comes back to the hall") {
        DirectoryModel model = floorsModel();
        model.apply(UiCommand::Activate);
        model.apply(UiCommand::Home);
        CHECK(model.apply(UiCommand::Activate) == command(DirectoryCommandKind::ChooseFloor, "gen4"));
        model.reopen();
        CHECK(model.focusedElement() == "dir-room-0");
    }
    SECTION("Mostra nella stanza: the card as it was, nothing asked again") {
        DirectoryModel model = cardModel(std::string(qc::kRoomPlaced));
        const std::uint64_t requests = model.cardRequest();
        model.apply(UiCommand::Right);
        CHECK(model.apply(UiCommand::Activate) == command(DirectoryCommandKind::ShowInRoom, games(5)[0].gameId));
        CHECK(model.cardGameId().empty());
        model.reopen();
        CHECK(model.view() == DirectoryView::Card);
        CHECK(model.cardGameId() == games(5)[0].gameId);
        CHECK(model.cardContent().details);
        CHECK(model.cardShowsRoom());
        CHECK(model.cardButton() == DirectoryCardButton::ShowInRoom);
        CHECK(model.focusedElement() == dir::kCardRoomId);
        CHECK(model.cardRequest() == requests);
        // The card's game's answers are taken again.
        GameCardContent content = model.cardContent();
        content.outcome = "crashed";
        model.setCard(content, std::string(qc::kRoomPlaced));
        CHECK(model.cardContent().outcome == "crashed");
    }
    SECTION("never opened: the floors, the walker's floor selected") {
        DirectoryModel model;
        model.setBuilding(building());
        model.reopen();
        CHECK(model.isOpen());
        CHECK(model.view() == DirectoryView::Floors);
        CHECK(model.focusedElement() == "dir-floor-1");
    }
}

TEST_CASE("ui/directory: every one-line text sits straight in the paragraph that clips it",
          "[ui][directory][documents]") {
    // RmlUi ends a line in an ellipsis only for a text whose parent clips it with text-overflow, measured from the
    // parent's left edge (ElementText::GenerateGeometry): a text in a span, or after another element, is cut through
    // a glyph instead.
    const std::string rml = readAsset(dir::kDocumentPath);
    static const std::regex clipped(R"re(<p class="(row-title|row-meta|header-title|meta-rest)"[^>]*>(.*?)</p>)re");
    std::size_t lines = 0;
    for (auto it = std::sregex_iterator(rml.begin(), rml.end(), clipped); it != std::sregex_iterator(); ++it) {
        ++lines;
        INFO((*it)[0].str());
        CHECK((*it)[2].str().find('<') == std::string::npos);
    }
    // Every such paragraph was read (none spans lines).
    std::size_t paragraphs = 0;
    for (const std::string_view opening :
         {"<p class=\"row-title\"", "<p class=\"row-meta\"", "<p class=\"header-title\"", "<p class=\"meta-rest\""}) {
        for (std::size_t at = rml.find(opening); at != std::string::npos; at = rml.find(opening, at + 1)) {
            ++paragraphs;
        }
    }
    CHECK(lines == paragraphs);
    CHECK(lines >= 40);
    const std::string rcss = readAsset(dir::kStyleSheetPath);
    for (const std::string_view rule : {".row-title {", ".row-meta {", ".header-title {", ".meta-rest {"}) {
        INFO(rule);
        const std::size_t begin = rcss.find(rule);
        REQUIRE(begin != std::string::npos);
        const std::string body = rcss.substr(begin, rcss.find('}', begin) - begin);
        CHECK(contains(body, "overflow: hidden"));
        CHECK(contains(body, "white-space: nowrap"));
        CHECK(contains(body, "text-overflow: ellipsis"));
    }
}

TEST_CASE("ui/directory: the document has the ids, bindings, events and keys its screen uses",
          "[ui][directory][documents]") {
    const std::string rml = readAsset(dir::kDocumentPath);
    CHECK(contains(rml, "data-model=\"" + std::string(dir::kDataModel) + "\""));
    const std::string sheet = rv::core::pathToUtf8(rv::core::pathFromUtf8(dir::kStyleSheetPath).filename());
    CHECK(contains(rml, "href=\"" + sheet + "\""));
    for (const std::string_view id : {dir::kPanelId, dir::kListId, dir::kFloorsTabId, dir::kGamesTabId, dir::kCardId,
                                      dir::kCardPlayId, dir::kCardRoomId, dir::kCardBackId}) {
        INFO(id);
        CHECK(contains(rml, "id=\"" + std::string(id) + "\""));
    }
    // The rows' ids from the screen's bindings; the events of tabs, rows and card buttons.
    for (const std::string_view binding :
         {"data-attr-id=\"floor.elementId\"", "data-attr-id=\"room.elementId\"", "data-attr-id=\"game.elementId\"",
          "data-event-click=\"tab('floors')\"", "data-event-click=\"tab('games')\"",
          "data-event-click=\"row(floor.index)\"", "data-event-click=\"row(room.index)\"",
          "data-event-click=\"row(game.index)\"", "data-event-click=\"card('play')\"",
          "data-event-click=\"card('room')\"", "data-event-click=\"card('back')\"", "floor : floors", "room : rooms",
          "game : games", "system, i : room.systems", "{{ game.title }}", "{{ game.systemName }}",
          "{{ game.played }}", "{{ floor.rooms }}", "{{ floor.games }}", "{{ room.games }}", "{{ card.title }}",
          "{{ card.platform }}", "{{ card.year }}", "{{ card.developer }}", "{{ card.genre }}",
          "{{ card.players }}", "card.details", "card.verified", "card.outcomeKnown", "floor.current",
          "room.current", "game.favorite"}) {
        INFO(binding);
        CHECK(contains(rml, binding));
    }
    for (const std::string_view floor : dir::kFloorIds) {
        INFO(floor);
        CHECK(contains(rml, "floor.id == '" + std::string(floor) + "'"));
        CHECK(contains(rml, "shown.id == '" + std::string(floor) + "'"));
    }
    for (const std::string_view kind : dir::kRoomKinds) {
        INFO(kind);
        CHECK(contains(rml, "room.kind == '" + std::string(kind) + "'"));
    }
    for (const std::string_view state : dir::kGamesStates) {
        INFO(state);
        CHECK(contains(rml, "gamesState == '" + std::string(state) + "'"));
    }
    for (const std::string_view notice : dir::kNotices) {
        INFO(notice);
        CHECK(contains(rml, "notice == '" + std::string(notice) + "'"));
    }
    for (const std::string_view readiness : qc::kReadinessKeys) {
        INFO(readiness);
        CHECK(contains(rml, "card.readiness == '" + std::string(readiness) + "'"));
    }
    for (const std::string_view code : qc::kBlockerCodes) {
        INFO(code);
        CHECK(contains(rml, "card.blocker == '" + std::string(code) + "'"));
    }
    for (const std::string_view code : rv::ui::game_card::kOutcomeCodes) {
        INFO(code);
        CHECK(contains(rml, "card.outcome == '" + std::string(code) + "'"));
    }
    CHECK(contains(rml, "card.room == '" + std::string(qc::kRoomPlaced) + "'"));
    for (const std::string_view key : qc::kRoomUnplacedKeys) {
        INFO(key);
        CHECK(contains(rml, "card.room == '" + std::string(key) + "'"));
    }
    // Italian words, "n.d." for an empty field, a game without a title.
    for (const std::string_view word : {">Piani<", ">Preferiti e recenti<", ">Avvia<", ">Mostra nella stanza<",
                                        ">Indietro<", "Gioco senza titolo", "n.d."}) {
        INFO(word);
        CHECK(contains(rml, word));
    }

    const std::string rcss = readAsset(dir::kStyleSheetPath);
    CHECK(contains(rcss, ".row:focus {"));
    CHECK(contains(rcss, ".action:focus {"));
    CHECK(contains(rcss, ".tab:focus {"));
    CHECK(contains(rcss, "@media (theme: compact)"));
    CHECK_FALSE(contains(rcss, "transition"));
    CHECK_FALSE(contains(rcss, "px;"));
}

TEST_CASE("ui/directory: the card's sentences are the quick catalog's and the world card's",
          "[ui][directory][documents]") {
    const std::string directory = readAsset(dir::kDocumentPath);
    const std::string catalog = readAsset(qc::kDocumentPath);
    const std::string card = readAsset(rv::ui::game_card::kDocumentPath);
    // Since I4 step 5a the quick catalog's room sentences speak of the building too ('not-in-room', 'other-room'), so
    // the directory's card says every one of them alike.
    const auto compare = [&](const std::string& source, std::string_view key, std::size_t atLeast) {
        const auto expected = sentencesOf(source, key);
        const auto actual = sentencesOf(directory, key);
        CHECK(expected.size() >= atLeast);
        for (const auto& [condition, sentence] : expected) {
            INFO(condition);
            const auto found = actual.find(condition);
            REQUIRE(found != actual.end());
            CHECK(found->second == sentence);
        }
    };
    compare(catalog, "card.readiness", 6);
    compare(catalog, "card.blocker", 31);
    compare(catalog, "card.room", 7);
    compare(card, "card.outcome", 11);
}

TEST_CASE("ui/directory: tests/input_scripts/directory_panel.json on rv_ui_probe's model", "[ui][directory]") {
    namespace fixture = rv::probe::directory_fixture;
    auto script = rv::input::InputScript::loadFromFile(rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" /
                                                       "input_scripts" / "directory_panel.json");
    if (!script) {
        FAIL(script.error().toString());
    }
    // Played as rv_ui_probe plays it (--fixed-dt 0.016667, frames counted from 0, no device), the panel opened on its
    // floors at frame 0.
    DirectoryModel model;
    model.setBuilding(fixture::building());
    model.setGames(fixture::games());
    model.open(DirectoryTab::Floors);
    rv::input::InputSystem input;
    rv::input::InputScriptPlayer player(std::move(*script));
    rv::ui::NavInput nav;
    std::uint64_t servedCard = 0;
    std::vector<std::pair<std::uint64_t, DirectoryCommand>> commands;
    std::size_t checked = 0;
    for (std::uint64_t frame = 0; frame < fixture::kFrames; ++frame) {
        rv::input::ActionState actions = input.resolve(rv::input::RawInputFrame{});
        player.apply(frame, actions, input.settings().pressThreshold);
        rv::ui::UiCommandList list;
        nav.update(actions, 0.016667, list);
        for (const UiCommand uiCommand : list.commands()) {
            if (auto asked = model.apply(uiCommand)) {
                commands.emplace_back(frame, *asked);
            }
        }
        fixture::fillCard(model, servedCard);
        for (const fixture::Checkpoint& checkpoint : fixture::kCheckpoints) {
            if (checkpoint.frame != frame) {
                continue;
            }
            INFO("frame " << frame);
            ++checked;
            CHECK(model.isOpen() == checkpoint.open);
            if (checkpoint.open) {
                CHECK(model.tab() == checkpoint.tab);
                CHECK(model.view() == checkpoint.view);
                CHECK(model.focusedElement() == checkpoint.focus);
                if (checkpoint.view == DirectoryView::Card) {
                    // Filled by the app's answer to the card's request.
                    CHECK(model.cardContent().details);
                }
            }
        }
    }
    CHECK(checked == fixture::kCheckpoints.size());
    REQUIRE(commands.size() == 1);
    CHECK(commands[0].first == fixture::kShowInRoomFrame);
    CHECK(commands[0].second == command(DirectoryCommandKind::ShowInRoom, fixture::gameId(fixture::kShownGame)));
}

TEST_CASE("ui/directory: \"Modifica stanza\" above the floors edits the walker's room (I4b step 3)", "[ui][directory]") {
    // Without an editable room the floors behave as before: Up from the first floor reaches the tab bar.
    DirectoryModel plain = floorsModel();
    CHECK_FALSE(plain.editShown());
    plain.apply(UiCommand::Home);
    plain.apply(UiCommand::Up);
    CHECK(plain.tabsFocused());

    DirectoryBuilding editable = building();
    editable.editableRoom = "room:snes";
    DirectoryModel model;
    model.setBuilding(editable);
    model.open(DirectoryTab::Floors);
    CHECK(model.editShown());
    CHECK_FALSE(model.editFocused());
    CHECK(model.focusedElement() == "dir-floor-1");
    // Up from the first floor: "Modifica stanza"; Up again: the tab bar; Down: back to it; Down: the floors.
    model.apply(UiCommand::Home);
    model.apply(UiCommand::Up);
    CHECK(model.editFocused());
    CHECK(model.focusedElement() == dir::kEditRoomId);
    model.apply(UiCommand::Up);
    CHECK(model.tabsFocused());
    CHECK_FALSE(model.editFocused());
    model.apply(UiCommand::Down);
    CHECK(model.editFocused());
    model.apply(UiCommand::Down);
    CHECK_FALSE(model.editFocused());
    CHECK(model.focusedElement() == "dir-floor-0");
    // Activate on it: EditRoom for the walker's room, the panel closed.
    model.apply(UiCommand::Up);
    REQUIRE(model.editFocused());
    const std::optional<DirectoryCommand> edit = model.apply(UiCommand::Activate);
    REQUIRE(edit.has_value());
    CHECK(*edit == command(DirectoryCommandKind::EditRoom, "room:snes"));
    CHECK(rv::ui::toString(DirectoryCommandKind::EditRoom) == "edit-room");
    CHECK_FALSE(model.isOpen());
    // Not on the rooms nor the games: it shows above the floors only.
    model.open(DirectoryTab::Floors);
    model.apply(UiCommand::Activate);
    CHECK(model.view() == DirectoryView::Rooms);
    CHECK_FALSE(model.editShown());
    // A click on it does what Activate does.
    model.apply(UiCommand::Back);
    DirectoryClick click;
    click.kind = DirectoryClick::Kind::Edit;
    const std::optional<DirectoryCommand> clicked = model.click(click);
    REQUIRE(clicked.has_value());
    CHECK(clicked->kind == DirectoryCommandKind::EditRoom);
    // The room no longer editable (a ride, a launch): gone, the focus on the floors.
    DirectoryModel gone;
    gone.setBuilding(editable);
    gone.open(DirectoryTab::Floors);
    gone.apply(UiCommand::Home);
    gone.apply(UiCommand::Up);
    REQUIRE(gone.editFocused());
    gone.setBuilding(building());
    CHECK_FALSE(gone.editFocused());
    CHECK(gone.focusedElement() == "dir-floor-0");
}

TEST_CASE("ui/directory: the \"Modifiche\" tab asks the regenerations and the undo (I4b step 6)", "[ui][directory]") {
    DirectoryBuilding walker = building();
    walker.regenerableRoom = "room:snes";
    DirectoryModel model;
    model.setBuilding(walker);
    model.open(DirectoryTab::Floors);
    // The shoulders walk the three tabs round; the tab bar's Left and Right stop at its ends.
    model.apply(UiCommand::FilterNext);
    model.apply(UiCommand::FilterNext);
    CHECK(model.tab() == DirectoryTab::Changes);
    CHECK(model.view() == DirectoryView::Changes);
    model.apply(UiCommand::FilterNext);
    CHECK(model.tab() == DirectoryTab::Floors);
    model.apply(UiCommand::FilterPrevious);
    CHECK(model.tab() == DirectoryTab::Changes);
    model.apply(UiCommand::Up);
    REQUIRE(model.tabsFocused());
    CHECK(model.focusedElement() == dir::kChangesTabId);
    model.apply(UiCommand::Right);
    CHECK(model.tab() == DirectoryTab::Changes);
    model.apply(UiCommand::Left);
    CHECK(model.tab() == DirectoryTab::Games);
    model.apply(UiCommand::Right);
    model.apply(UiCommand::Down);
    CHECK_FALSE(model.tabsFocused());
    // Its rows: no "Modifica stanza" without an editable room.
    CHECK(model.actions() == std::vector<DirectoryAction>{DirectoryAction::RegenerateRoom,
                                                          DirectoryAction::RegenerateFloor,
                                                          DirectoryAction::RegenerateWorld, DirectoryAction::Undo});
    CHECK(model.rowCount() == 4);
    CHECK(model.focusedElement() == "dir-action-0");
    CHECK(rv::ui::toString(DirectoryAction::RegenerateFloor) == "regenerate-floor");
    // "Rigenera stanza": the walker's room, the panel closed.
    std::optional<DirectoryCommand> asked = model.apply(UiCommand::Activate);
    REQUIRE(asked.has_value());
    CHECK(*asked == command(DirectoryCommandKind::RegenerateRoom, "room:snes"));
    CHECK_FALSE(model.isOpen());
    CHECK(rv::ui::toString(DirectoryCommandKind::RegenerateRoom) == "regenerate-room");
    // "Rigenera piano": the walker's floor; "Rigenera mondo" and "Annulla ultima modifica" name nothing.
    model.reopen();
    CHECK(model.view() == DirectoryView::Changes);
    model.apply(UiCommand::Down);
    asked = model.apply(UiCommand::Activate);
    REQUIRE(asked.has_value());
    CHECK(*asked == command(DirectoryCommandKind::RegenerateFloor, "gen4"));
    model.open(DirectoryTab::Changes);
    model.apply(UiCommand::End);
    asked = model.apply(UiCommand::Activate);
    REQUIRE(asked.has_value());
    CHECK(*asked == command(DirectoryCommandKind::Undo));
    CHECK(rv::ui::toString(DirectoryCommandKind::Undo) == "undo");
    model.open(DirectoryTab::Changes);
    model.apply(UiCommand::End);
    model.apply(UiCommand::Up);
    asked = model.apply(UiCommand::Activate);
    REQUIRE(asked.has_value());
    CHECK(*asked == command(DirectoryCommandKind::RegenerateWorld));
    // In a hall: "Rigenera stanza" says why and does nothing; a click on it neither.
    DirectoryBuilding hall = building();
    hall.currentRoom.clear();
    DirectoryModel inHall;
    inHall.setBuilding(hall);
    inHall.open(DirectoryTab::Changes);
    CHECK_FALSE(inHall.actionEnabled(DirectoryAction::RegenerateRoom));
    CHECK(inHall.actionEnabled(DirectoryAction::RegenerateFloor));
    CHECK_FALSE(inHall.apply(UiCommand::Activate).has_value());
    CHECK(inHall.isOpen());
    DirectoryClick click;
    click.kind = DirectoryClick::Kind::Row;
    click.row = 0;
    CHECK_FALSE(inHall.click(click).has_value());
    // An editable room puts "Modifica stanza" first: EditRoom, as above the floors.
    DirectoryBuilding editable = walker;
    editable.editableRoom = "room:snes";
    model.setBuilding(editable);
    model.open(DirectoryTab::Changes);
    CHECK(model.actions().front() == DirectoryAction::EditRoom);
    CHECK(model.rowCount() == 5);
    asked = model.apply(UiCommand::Activate);
    REQUIRE(asked.has_value());
    CHECK(*asked == command(DirectoryCommandKind::EditRoom, "room:snes"));
    // Back from the tab closes the panel.
    model.open(DirectoryTab::Changes);
    asked = model.apply(UiCommand::Back);
    REQUIRE(asked.has_value());
    CHECK(asked->kind == DirectoryCommandKind::Close);
}

TEST_CASE("ui/directory: the \"Modifiche\" tab's selection follows its action while \"Modifica stanza\" comes and "
          "goes (I4b step 6's review)",
          "[ui][directory]") {
    DirectoryBuilding walker = building();
    walker.regenerableRoom = "room:snes";
    DirectoryBuilding editable = walker;
    editable.editableRoom = "room:snes";
    DirectoryModel model;
    model.setBuilding(walker);
    model.open(DirectoryTab::Changes);
    model.apply(UiCommand::Down);
    model.apply(UiCommand::Down);
    REQUIRE(model.actions()[model.selectedAction()] == DirectoryAction::RegenerateWorld);
    // "Modifica stanza" arrives above: "Rigenera mondo" stays selected, one row down.
    model.setBuilding(editable);
    CHECK(model.selectedAction() == 3);
    CHECK(model.actions()[model.selectedAction()] == DirectoryAction::RegenerateWorld);
    CHECK(model.focusedElement() == "dir-action-3");
    // It goes: the same action, one row up.
    model.setBuilding(walker);
    CHECK(model.actions()[model.selectedAction()] == DirectoryAction::RegenerateWorld);
    // "Modifica stanza" selected and gone: the first row.
    model.setBuilding(editable);
    model.apply(UiCommand::Home);
    REQUIRE(model.actions()[model.selectedAction()] == DirectoryAction::EditRoom);
    model.setBuilding(walker);
    CHECK(model.selectedAction() == 0);
    CHECK(model.actions()[model.selectedAction()] == DirectoryAction::RegenerateRoom);
}

TEST_CASE("ui/directory: the \"Modifiche\" tab's document: its tab, rows and notice (I4b step 6)",
          "[ui][directory][documents]") {
    const std::string rml = readAsset(dir::kDocumentPath);
    CHECK(contains(rml, "id=\"" + std::string(dir::kChangesTabId) + "\""));
    for (const std::string_view binding :
         {"data-event-click=\"tab('changes')\"", "action : actions", "data-attr-id=\"action.elementId\"",
          "data-event-click=\"row(action.index)\"", "view == 'changes'", "{{ actionRoomTitle }}", ">Modifiche<"}) {
        INFO(binding);
        CHECK(contains(rml, binding));
    }
    for (const std::string_view action : dir::kActions) {
        INFO(action);
        CHECK(contains(rml, "action.action == '" + std::string(action) + "'"));
    }
    for (const std::string_view word :
         {">Rigenera stanza<", ">Rigenera piano<", ">Rigenera mondo<", ">Annulla ultima modifica<"}) {
        INFO(word);
        CHECK(contains(rml, word));
    }
    // One short line each (I4b step 6's review: the five rows fit the list), the floor named by the directory's names.
    for (const std::string_view line :
         {">Sposta e fissa giochi e arredi<", ">Riallestisce {{ actionRoomTitle }}<", ">Riallestisce tutto l'edificio<",
          ">Torna a prima dell'ultima modifica<", ">Disponibile fuori dall'ascensore<",
          ">Riallestisce il piano {{ actionFloor }}<", "!actionFloorKnown"}) {
        INFO(line);
        CHECK(contains(rml, line));
    }
    for (const std::string_view floor : dir::kFloorIds) {
        INFO(floor);
        CHECK(contains(rml, "actionFloor == '" + std::string(floor) + "'"));
    }
    // The undo's button is a back chevron, not two sheets (which read as "copy").
    CHECK(contains(rml, "<span class=\"chevron\"></span>"));
    CHECK_FALSE(contains(rml, "sheet"));
}
