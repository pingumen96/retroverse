// The statistics screen's model (I7 step 6, ADR-0018 §9): its states, its sections, Back, clicks; the hours as they show;
// the content made from what the catalog read. Informazioni's rows (ADR-0018 §11).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>

#include <rv/app/statistics_data.hpp>
#include <rv/catalog/statistics.hpp>
#include <rv/ui/game_info_model.hpp>
#include <rv/ui/statistics_model.hpp>

using rv::ui::StatisticsCommand;
using rv::ui::StatisticsContent;
using rv::ui::StatisticsModel;
using rv::ui::StatisticsState;
using rv::ui::StatisticsTab;
using rv::ui::UiCommand;

TEST_CASE("ui/statistics: it opens asking for the figures, and says what it holds", "[ui][statistics]") {
    StatisticsModel model;
    model.open();
    CHECK(model.state() == StatisticsState::Loading);
    CHECK(model.takeRequest() == StatisticsCommand::Load);
    CHECK_FALSE(model.takeRequest());
    StatisticsContent played;
    played.empty = false;
    played.sessions = 3;
    model.setContent(played);
    CHECK(model.state() == StatisticsState::Listed);
    model.setContent(StatisticsContent{});
    CHECK(model.state() == StatisticsState::Empty);
    model.setUnavailable();
    CHECK(model.state() == StatisticsState::Unavailable);
    CHECK(model.content().sessions == 0);
}

TEST_CASE("ui/statistics: the sections step without wrapping; a click goes to one; Back closes", "[ui][statistics]") {
    StatisticsModel model;
    CHECK_FALSE(model.apply(UiCommand::Right).has_value()); // closed: nothing
    CHECK(model.tab() == StatisticsTab::Summary);
    model.open();
    static_cast<void>(model.apply(UiCommand::Left));
    CHECK(model.tab() == StatisticsTab::Summary);
    static_cast<void>(model.apply(UiCommand::FilterNext));
    CHECK(model.tab() == StatisticsTab::MostPlayed);
    static_cast<void>(model.apply(UiCommand::Right));
    static_cast<void>(model.apply(UiCommand::Right));
    static_cast<void>(model.apply(UiCommand::Right));
    static_cast<void>(model.apply(UiCommand::Right));
    CHECK(model.tab() == StatisticsTab::Statuses);
    static_cast<void>(model.apply(UiCommand::FilterPrevious));
    CHECK(model.tab() == StatisticsTab::Systems);
    const auto revision = model.revision();
    static_cast<void>(model.apply(UiCommand::Down)); // the screen scrolls: nothing here
    CHECK(model.revision() == revision);
    model.click(StatisticsTab::Recent);
    CHECK(model.tab() == StatisticsTab::Recent);
    CHECK(model.apply(UiCommand::Back) == StatisticsCommand::Close);
    CHECK_FALSE(model.isOpen());
    model.open();
    CHECK(model.tab() == StatisticsTab::Summary); // it opens on the summary again
    CHECK(model.clickClose() == StatisticsCommand::Close);
    for (const StatisticsTab tab : rv::ui::kStatisticsTabs) {
        CHECK(rv::ui::statisticsTabFromName(rv::ui::toString(tab)) == tab);
    }
    CHECK_FALSE(rv::ui::statisticsTabFromName("hours"));
}

TEST_CASE("ui/statistics: the hours show one decimal with a comma, a short game never reads as none", "[ui][statistics]") {
    using rv::ui::estimatedHours;
    CHECK(estimatedHours(0) == "0");
    CHECK(estimatedHours(-5000) == "0");
    CHECK(estimatedHours(1000) == "0,1");
    CHECK(estimatedHours(360'000) == "0,1");
    CHECK(estimatedHours(3'600'000) == "1,0");
    CHECK(estimatedHours(5'400'000) == "1,5");
    CHECK(estimatedHours(3'600'000 * 1234LL + 1'800'000) == "1234,5");
    CHECK(estimatedHours(3'598'000) == "1,0"); // rounded, in integers
}

TEST_CASE("app/statistics: the content from what the catalog read", "[app][statistics]") {
    using namespace rv::catalog;
    PlayStatistics read;
    read.sessions = 2;
    read.crashed = 1;
    read.played = std::chrono::hours(3);
    read.gamesPlayed = 1;
    read.games = 7;
    PlayedGame game;
    game.id = GameId::fromValue(1).value();
    game.system = SystemName::parse("gb").value();
    game.title = "Cobalt";
    game.sessions = 2;
    game.played = std::chrono::hours(3);
    game.lastPlayed = std::chrono::system_clock::time_point{};
    game.esdePlayed = true;
    read.mostPlayed.push_back(game);
    read.mostRecent.push_back(game);
    SystemStatistics system;
    system.system = game.system;
    system.sessions = 2;
    read.systems.push_back(system);
    read.statuses[1].status = GameStatus::InProgress;
    read.statuses[1].games = 1;

    const auto day = [](UtcTime) { return std::optional<rv::ui::DirectoryDate>(rv::ui::DirectoryDate{2026, 11, 2}); };
    const StatisticsContent content = rv::app::statisticsContentOf(read, day);
    CHECK_FALSE(content.empty);
    CHECK(content.sessions == 2);
    CHECK(content.hours == "3,0");
    CHECK(content.crashed == 1);
    REQUIRE(content.mostPlayed.size() == 1);
    CHECK(content.mostPlayed[0].system == "gb"); // no full name: its name
    CHECK(content.mostPlayed[0].date == "02/11/2026");
    CHECK(content.recent[0].esde);
    CHECK(content.statuses.size() == 4);
    CHECK(content.statuses[1].status == "in-progress");
    CHECK(content.statuses[1].games == 1);
    CHECK(rv::app::statisticsContentOf(PlayStatistics{}, day).empty);
}

TEST_CASE("ui/game info: the rows follow what the game has", "[ui][media_viewer]") {
    using rv::ui::GameInfoContent;
    using rv::ui::InfoRow;
    GameInfoContent content;
    CHECK(rv::ui::infoRowsOf(content).empty()); // a catalog without them, nothing else to focus
    content.versions.push_back(rv::ui::OtherVersion{"1", "One", "USA"});
    content.versions.push_back(rv::ui::OtherVersion{"2", "Two", "Europe"});
    auto rows = rv::ui::infoRowsOf(content);
    REQUIRE(rows.size() == 2);
    CHECK(rows[1] == InfoRow{InfoRow::Kind::Version, 1});
    content.controls = rv::ui::GameInfoControls{};
    rows = rv::ui::infoRowsOf(content);
    REQUIRE(rows.size() == 4);
    CHECK(rows[0].kind == InfoRow::Kind::Status);
    CHECK(rows[1].kind == InfoRow::Kind::Emulator);
    CHECK(rows[2] == InfoRow{InfoRow::Kind::Version, 0});
}
