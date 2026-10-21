#pragma once

// GPU-free, header only: the directory's fixed model of rv_ui_probe (I4 step 7a), shared with the tests that check it:
// tests/smoke/test_directory_panel.cpp reads the probe's report and screenshots, tests/unit/ui/test_directory.cpp plays
// tests/input_scripts/directory_panel.json on the same model without the GPU. A building of six floors (the walker in
// room:snes on gen4, whose shared room has five sections of long names), twelve favourites and recent games (a long
// title, a favourite on a long system name), the card's content the app would give each, and what the script leads to
// at its checkpoints. Invented systems, titles and companies only.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/ui/directory_model.hpp>
#include <rv/ui/game_card_model.hpp>
#include <rv/ui/quick_catalog_room.hpp>

namespace rv::probe::directory_fixture {

inline ui::DirectoryRoom room(std::string id, std::string kind,
                              std::vector<std::pair<std::string, std::string>> systems, std::uint32_t games) {
    ui::DirectoryRoom result;
    result.id = std::move(id);
    result.kind = std::move(kind);
    for (auto& [name, fullName] : systems) {
        result.systems.push_back(ui::DirectorySystem{std::move(name), std::move(fullName)});
    }
    result.games = games;
    return result;
}

/// Six floors in the elevator's order, the walker in room:snes on gen4 (the third floor of the list); gen4's shared
/// room holds five sections whose full names are long, as ES-DE's are.
inline ui::DirectoryBuilding building() {
    ui::DirectoryBuilding result;
    result.floors.push_back(
        {"ground", 0, {room("area:arcade", "arcade", {{"mame", "Lumen Arcade Boards"}, {"fbneo", "Neon Cabinets"}}, 15)}});
    result.floors.push_back({"gen3",
                             2,
                             {room("room:nes", "console", {{"nes", "Lumen 8 Home System"}}, 412),
                              room("room:mastersystem", "console", {{"mastersystem", "Corona Master Console"}}, 96)}});
    result.floors.push_back(
        {"gen4",
         3,
         {room("room:megadrive", "console", {{"megadrive", "Corona 16 Drive"}}, 79),
          room("room:snes", "console", {{"snes", "Lumen 16 Home System"}}, 50),
          room("area:shared:gen4", "shared",
               {{"neogeo", "Neon Geo Advanced Entertainment System"},
                {"segacd", "Corona Compact Disc Expansion for the 16 Drive"},
                {"lumenchrome", "Lumen TurboChrome Compact Disc Interface"},
                {"prismnet", "Prism Interactive Multimedia Home Computer"},
                {"ambrapad", "Ambra Graphics Enhancement Console"}},
               41)}});
    result.floors.push_back({"gen5", 4, {room("room:psx", "console", {{"psx", "Prism Disc Station"}}, 140)}});
    result.floors.push_back(
        {"handheld",
         7,
         {room("room:gb", "console", {{"gb", "Pocket Lumen"}}, 1392),
          room("area:shared:handheld", "shared", {{"pokemini", "Pocket Pixel Mini"}, {"psp", "Lumen Portable Plus"}},
               23)}});
    result.floors.push_back({"computer", 8, {room("room:amiga", "console", {{"amiga", "Ambra 500 Computer"}}, 300)}});
    result.currentFloor = "gen4";
    result.currentRoom = "room:snes";
    return result;
}

struct FixtureGame {
    std::string_view title;
    std::string_view system;
    std::string_view systemName;
    bool favorite;
    /// Day, month, year last played; 0 never.
    int day, month, year;
};

inline constexpr std::array<FixtureGame, 12> kGames{{
    {"Amber Lantern", "snes", "Lumen 16 Home System", true, 19, 10, 2026},
    {"Bramble Burrow Rally", "segacd", "Corona Compact Disc Expansion for the 16 Drive", true, 2, 9, 2026},
    {"Cobalt Comet", "psx", "Prism Disc Station", true, 0, 0, 0},
    {"Dusk Harbor", "gb", "Pocket Lumen", true, 0, 0, 0},
    {"Ember Vale", "snes", "Lumen 16 Home System", false, 20, 10, 2026},
    {"Fern Hollow Quest", "amiga", "Ambra 500 Computer", false, 18, 10, 2026},
    {"Glass Garden", "nes", "Lumen 8 Home System", false, 14, 10, 2026},
    {"Harbor Lights Deluxe Edition: The Long Voyage Home", "psx", "Prism Disc Station", false, 30, 9, 2026},
    {"Indigo Isle", "mame", "Lumen Arcade Boards", false, 21, 9, 2026},
    {"Jade Jetty", "gb", "Pocket Lumen", false, 3, 8, 2026},
    {"Kestrel Keep", "pokemini", "Pocket Pixel Mini", false, 12, 7, 2026},
    {"Lark Lagoon", "mastersystem", "Corona Master Console", false, 1, 6, 2026},
}};

/// The GameId text of the fixture's game `index`.
inline std::string gameId(std::size_t index) {
    const std::string digits = std::to_string(index + 1);
    return "g" + std::string(16 - digits.size(), '0') + digits;
}

inline std::vector<ui::DirectoryGame> games() {
    std::vector<ui::DirectoryGame> result;
    for (std::size_t index = 0; index < kGames.size(); ++index) {
        const FixtureGame& game = kGames[index];
        ui::DirectoryGame row;
        row.gameId = gameId(index);
        row.title = std::string(game.title);
        row.system = std::string(game.system);
        row.systemName = std::string(game.systemName);
        row.favorite = game.favorite;
        if (game.year != 0) {
            row.played = ui::DirectoryDate{game.year, game.month, game.day};
        }
        result.push_back(std::move(row));
    }
    return result;
}

/// What the app would give the card of game `index`: its details, ready (verified for the even ones), placed in its
/// room for the even ones, a duplicate title for the odd ones.
inline std::pair<ui::GameCardContent, std::string> card(std::size_t index) {
    const FixtureGame& game = kGames[index];
    ui::GameCardContent content;
    content.gameId = gameId(index);
    content.title = std::string(game.title);
    content.platform = std::string(game.systemName);
    content.year = 1990 + static_cast<int>(index);
    content.developer = "Studio Mezzanotte";
    content.genre = index % 3 == 0 ? "Piattaforme" : "Avventura";
    content.players = "1-2";
    content.readiness = "ready";
    content.verified = index % 2 == 0;
    content.details = true;
    content.otherVersions = index % 2 == 0 ? 0 : 2;
    return {content, std::string(index % 2 == 0 ? ui::quick_catalog::kRoomPlaced : "duplicate-title")};
}

/// What the app does once a frame's commands are applied (the contract of step 7b): a card request it has not served
/// (`served`, the last cardRequest() answered) gets the content of the card's game (card()).
inline void fillCard(ui::DirectoryModel& model, std::uint64_t& served) {
    if (model.view() != ui::DirectoryView::Card || !model.isOpen() || model.cardRequest() == served) {
        return;
    }
    served = model.cardRequest();
    for (std::size_t index = 0; index < kGames.size(); ++index) {
        if (gameId(index) == model.cardGameId()) {
            auto [content, room] = card(index);
            model.setCard(std::move(content), std::move(room));
        }
    }
}

/// The directory as the script leaves it at a frame (after that frame's commands), and the screenshot taken there.
struct Checkpoint {
    std::uint32_t frame;
    bool open;
    ui::DirectoryTab tab;
    ui::DirectoryView view;
    std::string_view focus;
    /// The screenshot's file name in the case's folder; empty for none.
    std::string_view shot;
};

/// tests/input_scripts/directory_panel.json from the floors opened at frame 0: the floors (gen4 selected), a step down
/// and back, gen4's rooms (room:snes selected; the shared room's five long names), Back to the floors, the shoulder to
/// the games (the first selected), a page down and a step up (the long title selected), a page up and two steps down,
/// the third game's card (placed), Right to Mostra nella stanza, which leaves the panel.
inline constexpr std::array<Checkpoint, 9> kCheckpoints{{
    {20, true, ui::DirectoryTab::Floors, ui::DirectoryView::Floors, "dir-floor-2", "floors.png"},
    {35, true, ui::DirectoryTab::Floors, ui::DirectoryView::Floors, "dir-floor-3", ""},
    {70, true, ui::DirectoryTab::Floors, ui::DirectoryView::Rooms, "dir-room-2", "rooms.png"},
    {85, true, ui::DirectoryTab::Floors, ui::DirectoryView::Floors, "dir-floor-2", ""},
    {110, true, ui::DirectoryTab::Games, ui::DirectoryView::Games, "dir-game-0", ""},
    {145, true, ui::DirectoryTab::Games, ui::DirectoryView::Games, "dir-game-7", "games.png"},
    {200, true, ui::DirectoryTab::Games, ui::DirectoryView::Card, "dir-card-play", "card.png"},
    {225, true, ui::DirectoryTab::Games, ui::DirectoryView::Card, "dir-card-room", ""},
    {239, false, ui::DirectoryTab::Games, ui::DirectoryView::Card, "", ""},
}};

/// The frames the script runs (the last checkpoint's, plus one).
inline constexpr std::uint32_t kFrames = 240;
/// The command the script ends with, and its frame: Mostra nella stanza on the third game.
inline constexpr std::uint32_t kShowInRoomFrame = 230;
inline constexpr std::size_t kShownGame = 2;

} // namespace rv::probe::directory_fixture
