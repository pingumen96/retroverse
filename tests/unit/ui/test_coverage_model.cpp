// The media coverage screen's model (I7 step 8, ADR-0018 §14): the systems walked, a system opened and closed, the
// population changed with the focus kept on the same system, the clips of the system shown, the clicks; the shares'
// text.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include <rv/ui/coverage_model.hpp>

using rv::ui::CoverageClick;
using rv::ui::CoverageCommand;
using rv::ui::CoverageModel;
using rv::ui::CoverageTab;
using rv::ui::CoverageView;
using rv::ui::UiCommand;

namespace {

rv::ui::CoverageRow row(std::string name, std::uint64_t games) {
    rv::ui::CoverageRow made;
    made.name = name;
    made.fullName = "Full " + name;
    made.games = games;
    return made;
}

rv::ui::CoverageContent content() {
    rv::ui::CoverageContent made;
    made.empty = false;
    made.games.systems = {row("gb", 8), row("snes", 5), row("psx", 2)};
    made.scraped.systems = {row("psx", 2), row("gb", 3)};
    made.clips = {{"gb", "Tiny.mp4", "truncated"}, {"snes", "Alpha.mp4", "corrupt-data"}};
    return made;
}

} // namespace

TEST_CASE("ui/coverage: the systems walked, one opened, the population changed", "[ui][coverage]") {
    CoverageModel model;
    model.open();
    CHECK(model.state() == rv::ui::CoverageState::Loading);
    CHECK(model.takeRequest() == CoverageCommand::Load);
    CHECK_FALSE(model.takeRequest().has_value());
    model.setContent(content());
    CHECK(model.state() == rv::ui::CoverageState::Listed);
    CHECK(model.focusedElement() == "cv-row-0");
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.system()->name == "snes");
    CHECK_FALSE(model.apply(UiCommand::End).has_value());
    CHECK(model.system()->name == "psx");
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.focused() == 2);
    CHECK_FALSE(model.apply(UiCommand::Home).has_value());
    CHECK(model.system()->name == "gb");
    // Opened: its clips; the focus is no row's.
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == CoverageView::System);
    CHECK(model.focusedElement().empty());
    REQUIRE(model.systemClips().size() == 1);
    CHECK(model.systemClips().front().file == "Tiny.mp4");
    // The other population lists gb second: it stays on gb.
    CHECK_FALSE(model.apply(UiCommand::FilterNext).has_value());
    CHECK(model.tab() == CoverageTab::Scraped);
    CHECK(model.view() == CoverageView::System);
    CHECK(model.system()->name == "gb");
    CHECK(model.focused() == 1);
    // Back: the list; Back again closes.
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.view() == CoverageView::Systems);
    // snes is not among the scraped games: the list's first, the view the list.
    CHECK_FALSE(model.apply(UiCommand::FilterPrevious).has_value());
    CHECK_FALSE(model.apply(UiCommand::Down).has_value());
    CHECK(model.system()->name == "snes");
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK_FALSE(model.apply(UiCommand::Right).has_value());
    CHECK(model.view() == CoverageView::Systems);
    CHECK(model.system()->name == "psx");
    CHECK(model.apply(UiCommand::Back) == CoverageCommand::Close);
    CHECK_FALSE(model.isOpen());
}

TEST_CASE("ui/coverage: clicks press what the keys press; no catalog says so", "[ui][coverage]") {
    CoverageModel model;
    model.open();
    static_cast<void>(model.takeRequest());
    model.setContent(content());
    CHECK_FALSE(model.click(CoverageClick{CoverageClick::Kind::System, CoverageTab::Games, 2}).has_value());
    CHECK(model.view() == CoverageView::System);
    CHECK(model.system()->name == "psx");
    CHECK_FALSE(model.click(CoverageClick{CoverageClick::Kind::System, CoverageTab::Games, 0}).has_value());
    CHECK(model.system()->name == "psx"); // no row shows under a system's details
    CHECK_FALSE(model.click(CoverageClick{CoverageClick::Kind::Close, CoverageTab::Games, 0}).has_value());
    CHECK(model.view() == CoverageView::Systems);
    CHECK_FALSE(model.click(CoverageClick{CoverageClick::Kind::System, CoverageTab::Games, 9}).has_value());
    CHECK(model.view() == CoverageView::Systems);
    CHECK_FALSE(model.click(CoverageClick{CoverageClick::Kind::Tab, CoverageTab::Scraped, 0}).has_value());
    CHECK(model.tab() == CoverageTab::Scraped);
    CHECK(model.click(CoverageClick{CoverageClick::Kind::Close, CoverageTab::Games, 0}) == CoverageCommand::Close);

    model.open();
    model.setUnavailable();
    CHECK(model.state() == rv::ui::CoverageState::Unavailable);
    CHECK(model.system() == nullptr);
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.view() == CoverageView::Systems);
}

TEST_CASE("ui/coverage: a share as the screen shows it", "[ui][coverage]") {
    CHECK(rv::ui::shareText(std::nullopt) == "-");
    CHECK(rv::ui::shareText(0.0) == "0,0");
    CHECK(rv::ui::shareText(1.0) == "100,0");
    CHECK(rv::ui::shareText(0.909) == "90,9");
    CHECK(rv::ui::shareText(1.0 / 3.0) == "33,3");
}
