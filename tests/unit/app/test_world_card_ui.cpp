// The world's card in the app (I3 step 14a; ADR-0012 §5, rv/app/world_card_ui.hpp): its content from the catalog's
// entry of the inspected game (the quick catalog's row: readiness and blocker as its card shows them), the room's title
// until the catalog answered, the plan's other versions, the outcome of a launch that did not end normally; the card's
// command of a frame's actions and the interaction's entry of each button.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include <rv/app/world_card_ui.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>

using rv::app::CardAction;
using rv::catalog::GameId;
using rv::input::Action;
using rv::ui::CardButton;
using rv::ui::UiCommand;

namespace {

GameId game(std::uint64_t value) {
    return GameId::fromValue(value).value();
}

rv::input::ActionState pressing(Action action) {
    rv::input::ActionState state;
    state.setButton(action, true, true, false);
    return state;
}

} // namespace

TEST_CASE("app/world_card_ui: the card shows the catalog's row, the room's title until it comes", "[app][world_card]") {
    const GameId id = game(0x1234);
    const rv::app::LaunchMessage none;
    // Before the catalog answered: the room's title, the details to come.
    const rv::ui::GameCardContent early = rv::app::cardContentOf(id, "Trellis Tales", false, std::nullopt, 0, false,
                                                                 rv::app::InteractionNotice::None, none);
    CHECK(early.gameId == id.toString());
    CHECK(early.title == "Trellis Tales");
    CHECK_FALSE(early.details);
    CHECK(early.readiness.empty());
    CHECK(early.outcome.empty());

    rv::ui::CatalogEntry entry;
    entry.id = id.toString();
    entry.title = "Trellis Tales (Europe)";
    entry.platform = "Super Console";
    entry.year = 1994;
    entry.developer = "Studio Mezzanotte";
    entry.genre = "Puzzle";
    entry.players = "1-2";
    entry.state = "needs-configuration";
    entry.readiness = "requires-configuration";
    entry.blocker = "inject-file";
    entry.blockerDetail = "words the card never shows";
    entry.verified = false;
    const rv::ui::GameCardContent full =
        rv::app::cardContentOf(id, "Trellis Tales", true, entry, 2, true, rv::app::InteractionNotice::None, none);
    CHECK(full.details);
    CHECK(full.title == "Trellis Tales (Europe)");
    CHECK(full.platform == "Super Console");
    CHECK(full.year == 1994);
    CHECK(full.developer == "Studio Mezzanotte");
    CHECK(full.genre == "Puzzle");
    CHECK(full.players == "1-2");
    CHECK(full.state == "needs-configuration");
    CHECK(full.readiness == "requires-configuration");
    CHECK(full.blocker == "inject-file");
    CHECK(full.unavailable);
    CHECK(full.otherVersions == 2);
    CHECK(full.cover);
    // A row without a title keeps the room's.
    entry.title.clear();
    CHECK(rv::app::cardContentOf(id, "Trellis Tales", false, entry, 0, false, rv::app::InteractionNotice::None, none)
              .title == "Trellis Tales");
}

TEST_CASE("app/world_card_ui: a launch that did not end normally is told on the card", "[app][world_card]") {
    const GameId id = game(7);
    const rv::app::LaunchMessage crashed{std::string(rv::app::launch_outcome::kCrashed), "", "0xC0000005"};
    CHECK(rv::app::cardContentOf(id, "", false, std::nullopt, 0, false, rv::app::InteractionNotice::LaunchOutcome,
                                 crashed)
              .outcome == "crashed");
    const rv::app::LaunchMessage returned{std::string(rv::app::launch_outcome::kReturned), "", ""};
    CHECK(rv::app::cardContentOf(id, "", false, std::nullopt, 0, false, rv::app::InteractionNotice::LaunchOutcome,
                                 returned)
              .outcome.empty());
    CHECK(rv::app::cardContentOf(id, "", false, std::nullopt, 0, false, rv::app::InteractionNotice::ItemUnavailable,
                                 crashed)
              .outcome.empty());
    // The outcome codes the card has sentences for are rv_app's.
    for (const std::string_view code : rv::ui::game_card::kOutcomeCodes) {
        INFO(code);
        CHECK((code == rv::app::launch_outcome::kQuickExit || code == rv::app::launch_outcome::kProcessesRemained ||
               code == rv::app::launch_outcome::kLeftRunning || code == rv::app::launch_outcome::kCrashed ||
               code == rv::app::launch_outcome::kFailedToStart || code == rv::app::launch_outcome::kNotConfigured ||
               code == rv::app::launch_outcome::kEmulatorMissing || code == rv::app::launch_outcome::kCoreMissing ||
               code == rv::app::launch_outcome::kRomMissing || code == rv::app::launch_outcome::kNotReady));
    }
}

TEST_CASE("app/world_card_ui: the other versions of each placed title are its duplicate-title entries",
          "[app][world_card]") {
    rv::world::WorldPlan plan;
    const auto unplaced = [](std::uint64_t gameValue, rv::world::UnplacedReason reason, std::uint64_t representative) {
        rv::world::UnplacedGame entry;
        entry.game = game(gameValue);
        entry.reason = reason;
        if (representative != 0) {
            entry.representative = game(representative);
        }
        return entry;
    };
    plan.unplaced.push_back(unplaced(10, rv::world::UnplacedReason::DuplicateTitle, 1));
    plan.unplaced.push_back(unplaced(11, rv::world::UnplacedReason::DuplicateTitle, 1));
    plan.unplaced.push_back(unplaced(12, rv::world::UnplacedReason::DuplicateTitle, 2));
    plan.unplaced.push_back(unplaced(13, rv::world::UnplacedReason::NoCover, 0));
    plan.unplaced.push_back(unplaced(14, rv::world::UnplacedReason::Capacity, 0));
    const auto counts = rv::app::otherVersionCounts(plan);
    CHECK(counts.size() == 2);
    CHECK(counts.at(1) == 2);
    CHECK(counts.at(2) == 1);
    CHECK(rv::app::otherVersionCounts(rv::world::WorldPlan{}).empty());
}

TEST_CASE("app/world_card_ui: the card's commands of a frame and the interaction's entry of each button",
          "[app][world_card]") {
    CHECK(rv::app::cardCommandOf(pressing(Action::UiUp)) == UiCommand::Up);
    CHECK(rv::app::cardCommandOf(pressing(Action::UiDown)) == UiCommand::Down);
    CHECK(rv::app::cardCommandOf(pressing(Action::UiLeft)) == UiCommand::Left);
    CHECK(rv::app::cardCommandOf(pressing(Action::UiRight)) == UiCommand::Right);
    CHECK(rv::app::cardCommandOf(pressing(Action::UiActivate)) == UiCommand::Activate);
    // The shortcuts are the interaction's own; held buttons are no press.
    CHECK_FALSE(rv::app::cardCommandOf(pressing(Action::UiMedia)).has_value());
    CHECK_FALSE(rv::app::cardCommandOf(pressing(Action::Confirm)).has_value());
    rv::input::ActionState held;
    held.setButton(Action::UiRight, true, false, false);
    CHECK_FALSE(rv::app::cardCommandOf(held).has_value());

    CHECK(rv::app::cardActionOf(CardButton::Play) == CardAction::Launch);
    CHECK(rv::app::cardActionOf(CardButton::Media) == CardAction::Media);
    CHECK(rv::app::cardActionOf(CardButton::Info) == CardAction::Info);
    CHECK(rv::app::cardActionOf(CardButton::Back) == CardAction::Back);
    CHECK(rv::app::cardActionOf(CardButton::Move) == CardAction::Move);
}
