#pragma once

// GPU-free (part of rv_app_cli, header only): what the world's card (I3 step 14a; ADR-0012 §5; rv_app's WorldCard)
// shows for the inspected item, from the catalog's entry of its game (AppCatalog::gameEntry: the quick catalog's row,
// readiness and blocker included), the room's item, the plan's other versions of its title and the interaction's
// notice; and which card button a frame's actions press. The words are the document's (assets/ui/game_card.rml).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <rv/app/launch_outcome.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>
#include <rv/ui/catalog_source.hpp>
#include <rv/ui/game_card_model.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::app {

/// The other versions of each placed title in `plan` (its duplicate-title entries, by their representative's GameId
/// value).
[[nodiscard]] inline std::map<std::uint64_t, std::uint32_t> otherVersionCounts(const world::WorldPlan& plan) {
    std::map<std::uint64_t, std::uint32_t> counts;
    for (const world::UnplacedGame& unplaced : plan.unplaced) {
        if (unplaced.reason == world::UnplacedReason::DuplicateTitle && unplaced.representative.isValid()) {
            ++counts[unplaced.representative.value()];
        }
    }
    return counts;
}

/// The card's content for `game`: the catalog's entry when it answered (its title, platform, year, developer, genre,
/// players, readiness and first blocker as the quick catalog shows them), else the room's title alone with the details
/// still to come; the item's availability, the other versions, whether the cover's texture is registered, and the
/// outcome of the last launch from the card when it did not end normally (InteractionNotice::LaunchOutcome).
[[nodiscard]] inline ui::GameCardContent cardContentOf(const catalog::GameId& game, std::string_view roomTitle,
                                                       bool unavailable, const std::optional<ui::CatalogEntry>& entry,
                                                       std::uint32_t otherVersions, bool cover,
                                                       InteractionNotice notice, const LaunchMessage& outcome) {
    ui::GameCardContent content;
    content.gameId = game.toString();
    content.title = std::string(roomTitle);
    content.unavailable = unavailable;
    if (entry) {
        content.details = true;
        if (!entry->title.empty()) {
            content.title = entry->title;
        }
        content.platform = entry->platform;
        content.year = entry->year;
        content.developer = entry->developer;
        content.genre = entry->genre;
        content.players = entry->players;
        content.state = entry->state;
        content.readiness = entry->readiness;
        content.blocker = entry->blocker;
        content.verified = entry->verified;
    }
    content.otherVersions = otherVersions;
    content.cover = cover;
    if (notice == InteractionNotice::LaunchOutcome && outcome.code != launch_outcome::kReturned) {
        content.outcome = outcome.code;
    }
    return content;
}

/// The card's command of a frame's actions in Inspect (the arrows: the focus; ui_activate: the focused button), the
/// first that was pressed: the directions in the order up, down, left, right, then ui_activate. The shortcuts
/// (ui_media, ui_info, ui_back, the pad's Start) are WorldInteraction's own.
[[nodiscard]] inline std::optional<ui::UiCommand> cardCommandOf(const input::ActionState& actions) noexcept {
    using input::Action;
    if (actions.pressed(Action::UiUp)) {
        return ui::UiCommand::Up;
    }
    if (actions.pressed(Action::UiDown)) {
        return ui::UiCommand::Down;
    }
    if (actions.pressed(Action::UiLeft)) {
        return ui::UiCommand::Left;
    }
    if (actions.pressed(Action::UiRight)) {
        return ui::UiCommand::Right;
    }
    if (actions.pressed(Action::UiActivate)) {
        return ui::UiCommand::Activate;
    }
    return std::nullopt;
}

/// The interaction's entry for a card button.
[[nodiscard]] constexpr CardAction cardActionOf(ui::CardButton button) noexcept {
    switch (button) {
    case ui::CardButton::Play:
        return CardAction::Launch;
    case ui::CardButton::Media:
        return CardAction::Media;
    case ui::CardButton::Info:
        return CardAction::Info;
    case ui::CardButton::Back:
        return CardAction::Back;
    }
    return CardAction::Back;
}

} // namespace rv::app
