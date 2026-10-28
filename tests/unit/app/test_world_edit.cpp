// The editor's GPU-free side (I4b step 3; ADR-0015 §4, §6): the edited room's targets (its items and free slots) and
// steps (empty slots included), the editor's interaction (its modes, transitions and input context: Browse, Holding,
// Busy, Menu, Saving; "Sposta" entering with the item held; Back's levels), the draft's history (each operation kept,
// a refusal or a no-op not, undo back to the base byte for byte), its save behind a snapshot (action "edit", undone by
// world::undoLastAction to the plan's SHA-256 before) and a save replayed over a plan changed meanwhile (applied, or
// refused with nothing written).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <rv/app/world_edit_draft.hpp>
#include <rv/app/world_edit_interaction.hpp>
#include <rv/app/world_edit_room.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/overrides.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_edit.hpp>
#include <rv/world/world_snapshots.hpp>
#include <rv/world/world_store.hpp>

#include "../world/plan_scene_support.hpp"
#include "../world/world_generation_support.hpp"
#include "support/temp_dir.hpp"

namespace {

using rv::app::EditDraft;
using rv::app::EditFrame;
using rv::app::EditFrameResult;
using rv::app::EditInteraction;
using rv::app::EditMenuButton;
using rv::app::EditMode;
using rv::app::EditRoom;
using rv::catalog::GameId;
using rv::input::Action;
using rv::input::ActionState;
using rv::world::EditOperation;
using rv::world::EditedWorld;
using rv::world::PickId;
using rv::world::Placement;
using rv::world::SlotId;
using rv::world::WorldPlan;

constexpr std::uint64_t kWorldSeed = 102938;
const rv::world::PinProvenance kProvenance{rv::world::PinOrigin::Editor, "2026-10-28T10:00:00.000Z"};

/// A fresh SNES room of 80 titles: spine-out shelves partly free, the face-out slots full.
const rv::world::GenerationResult& room80() {
    static const rv::world::GenerationResult result = rv::test::world::generateRoom(80, 57, kWorldSeed);
    return result;
}

std::shared_ptr<const rv::world::WorldData> unowned(const rv::world::WorldData& data) {
    return std::shared_ptr<const rv::world::WorldData>(std::shared_ptr<const rv::world::WorldData>(), &data);
}

std::shared_ptr<const rv::world::EditData> snesEditData() {
    auto data = std::make_shared<rv::world::EditData>();
    data->config = rv::test::world::roomConfig(kWorldSeed);
    data->building = rv::test::world::snesData().building;
    data->rooms.emplace(room80().plan.rooms.front().id, unowned(rv::test::world::snesData()));
    return data;
}

/// The layout of `plan`'s room (the shipped scene data, invented titles).
rv::world::CollectionLayout layoutOf(const WorldPlan& plan) {
    const rv::world::RoomId room = plan.rooms.front().id;
    auto data = rv::world::loadSceneData(rv::test::world::sourceAssetRoot(), plan, room);
    REQUIRE(data.has_value());
    auto layout = rv::world::collectionLayout(plan, room, *data, rv::test::world::inventedTitles(plan));
    REQUIRE(layout.has_value());
    return std::move(*layout);
}

std::unique_ptr<EditRoom> editRoomOf(const WorldPlan& plan) {
    auto room = EditRoom::build(std::make_shared<const rv::world::CollectionLayout>(layoutOf(plan)));
    REQUIRE(room.has_value());
    return std::move(*room);
}

/// The first item of `room` whose next slot to the right is free, and that free slot.
std::pair<PickId, PickId> itemBesideFree(const EditRoom& room) {
    std::pair<PickId, PickId> found{rv::world::kNoPick, rv::world::kNoPick};
    for (const rv::world::CollectionItem& item : room.layout().items) {
        const auto next = room.step(item.pickId, rv::world::StepDirection::Right);
        if (next && room.freeSlot(*next) != nullptr) {
            found = {item.pickId, *next};
            break;
        }
    }
    REQUIRE(found.first != rv::world::kNoPick);
    return found;
}

ActionState press(std::initializer_list<Action> actions) {
    ActionState state;
    for (const Action action : actions) {
        state.setButton(action, true, true, false);
    }
    return state;
}

/// One frame of the editor with `actions` (none: nothing pressed) and the reticle on `pick` (kNoPick: nothing).
EditFrameResult frame(EditInteraction& editor, const ActionState* actions, PickId pick) {
    EditFrame input;
    const ActionState none;
    input.actions = actions != nullptr ? actions : &none;
    input.deltaSeconds = 0.016667f;
    input.hover = rv::world::HoverSample{pick, pick != rv::world::kNoPick, editor.hovered() == pick};
    return editor.update(input);
}

/// The reticle rests on `pick` long enough for the hover to take it (world::kHoverChangeFrames).
void aim(EditInteraction& editor, PickId pick) {
    for (int i = 0; i < rv::world::kHoverChangeFrames + 1; ++i) {
        static_cast<void>(frame(editor, nullptr, pick));
    }
    REQUIRE(editor.target().has_value());
    REQUIRE(editor.target()->pick == pick);
}

EditFrameResult pressOn(EditInteraction& editor, std::initializer_list<Action> actions, PickId pick) {
    const ActionState state = press(actions);
    return frame(editor, &state, pick);
}

const Placement* placementOf(const WorldPlan& plan, const GameId& game) {
    const auto found = std::find_if(plan.placements.begin(), plan.placements.end(),
                                    [&](const Placement& placement) { return placement.game == game; });
    return found != plan.placements.end() ? &*found : nullptr;
}

/// A world folder with room80()'s plan saved, read back as the app reads it.
struct SavedWorld {
    rv::test::TempDir dir{"rv-world-edit"};
    rv::app::WorldPaths paths;
    rv::world::WorldFiles files;

    SavedWorld() {
        paths.assetRoot = rv::test::world::assetRoot();
        paths.userDataRoot = dir.path();
        paths.configDir = dir.file("config");
        paths.cacheDir = dir.file("cache");
        files = rv::world::worldFiles(paths.userDataRoot, rv::test::world::library());
        auto lock = rv::world::WorldLock::acquire(files);
        REQUIRE(lock.has_value());
        REQUIRE(rv::world::savePlan(*lock, room80()).has_value());
    }

    [[nodiscard]] rv::app::EditBase base() const {
        rv::app::StoredWorldPlan stored = rv::app::readWorldPlan(paths, rv::test::world::library());
        REQUIRE(stored.stored.has_value());
        auto overrides = rv::world::loadOverrides(files.overrides);
        REQUIRE(overrides.has_value());
        return rv::app::EditBase{EditedWorld{stored.stored->plan, *overrides}, stored.stored->digest};
    }

    [[nodiscard]] std::string planDigest() const {
        auto stored = rv::world::readStoredPlan(files.plan);
        REQUIRE(stored.has_value());
        return stored->digest;
    }

    [[nodiscard]] rv::app::EditSaveOptions saveOptions() const {
        rv::app::EditSaveOptions options;
        options.paths = paths;
        options.library = rv::test::world::library();
        options.writer = "a test (editor)";
        options.provenance = kProvenance;
        return options;
    }
};

} // namespace

TEST_CASE("app/world_edit: the edited room's targets are its items and its free slots, the steps reach both",
          "[app][world_edit]") {
    const std::unique_ptr<EditRoom> room = editRoomOf(room80().plan);
    const rv::world::CollectionLayout& layout = room->layout();
    REQUIRE_FALSE(layout.freeSlots.empty());
    // Every item and every free slot is a target; none is both; each slot of the navigation is one or neither.
    CHECK(room->index().targetCount() == layout.items.size() + layout.freeSlots.size());
    std::set<PickId> picks;
    for (const rv::world::CollectionItem& item : layout.items) {
        const auto target = room->target(item.pickId);
        REQUIRE(target.has_value());
        CHECK(target->game == item.game);
        CHECK_FALSE(target->free());
        CHECK(room->pickOf(item.game) == item.pickId);
        picks.insert(item.pickId);
    }
    for (const rv::world::FreeSlot& free : layout.freeSlots) {
        INFO(free.slot.toString());
        CHECK(picks.insert(free.pickId).second);
        const auto target = room->target(free.pickId);
        REQUIRE(target.has_value());
        CHECK(target->free());
        CHECK(target->slot == free.slot);
        CHECK(free.pickId == *rv::world::slotPickId(rv::test::world::snesData().roomTemplate, free.slot));
        CHECK(room->focusPose(free.pickId, rv::app::FocusViewer{}).has_value());
    }
    CHECK_FALSE(room->target(rv::world::kNoPick).has_value());
    // The steps of Focus over every slot that holds an item or is free: from an item to the free slot beside it, and
    // back.
    const auto [item, free] = itemBesideFree(*room);
    CHECK(room->step(free, rv::world::StepDirection::Left) == item);
    // A free slot's pick is the slot's: a game moved there takes that pick id.
    const rv::world::FreeSlot* slot = room->freeSlot(free);
    REQUIRE(slot != nullptr);
    EditedWorld world{room80().plan, {}};
    const GameId game = room->target(item)->game.value();
    auto moved = rv::world::applyEdit(world, EditOperation::move(game, slot->slot), *snesEditData(), kProvenance);
    REQUIRE(moved.has_value());
    REQUIRE_FALSE(moved->refused);
    const std::unique_ptr<EditRoom> after = editRoomOf(world.plan);
    CHECK(after->pickOf(game) == free);
    CHECK(after->freeSlot(item) != nullptr);
}

TEST_CASE("app/world_edit: the dressed free slots are the plan's dressing", "[app][world_edit]") {
    for (const std::string name : {"plan-15-seed-102938", "plan-890-seed-7"}) {
        INFO(name);
        const rv::test::world::PlanCase& entry = rv::test::world::planCase(name);
        const rv::world::CollectionLayout layout = rv::test::world::layoutOf(entry);
        std::set<SlotId> taken;
        for (const Placement& placement : entry.plan.placements) {
            taken.insert(placement.slot);
        }
        for (const rv::world::FreeSlot& free : layout.freeSlots) {
            CHECK_FALSE(taken.contains(free.slot));
            const bool covered = std::any_of(entry.plan.dressing.begin(), entry.plan.dressing.end(), [&](const auto& span) {
                return span.level == free.slot.levelId() && free.slot.index() >= span.firstSlot &&
                       free.slot.index() < span.firstSlot + span.count;
            });
            CHECK(free.dressed == covered);
        }
    }
}

TEST_CASE("app/world_edit: the editor's context reads the walk, the look and its own actions",
          "[app][world_edit][world_interaction]") {
    using rv::app::WorldContext;
    CHECK(rv::app::worldContextFor(rv::app::WorldState::Edit) == WorldContext::Edit);
    for (const Action action : {Action::MoveForward, Action::MoveRight, Action::LookX, Action::LookDeltaY,
                                Action::Sprint, Action::Interact, Action::Back, Action::Confirm, Action::EditPin,
                                Action::UiUp, Action::UiRight, Action::UiActivate, Action::Screenshot}) {
        INFO(rv::input::actionName(action));
        CHECK(rv::app::readsAction(WorldContext::Edit, action));
    }
    for (const Action action : {Action::QuickCatalog, Action::Map, Action::Menu, Action::UiMedia, Action::UiInfo,
                                Action::UiPageDown}) {
        INFO(rv::input::actionName(action));
        CHECK_FALSE(rv::app::readsAction(WorldContext::Edit, action));
    }
    // The walker walks and looks; a held arrow steps instead of walking.
    ActionState walking;
    walking.setAxis(Action::MoveForward, 1.0f);
    walking.setAxis(Action::LookX, 0.5f);
    ActionState walked = rv::app::walkerActionsFor(WorldContext::Edit, walking);
    CHECK(walked.axis(Action::MoveForward) == 1.0f);
    CHECK(walked.axis(Action::LookX) == 0.5f);
    walking.setButton(Action::UiUp, true, true, false);
    walked = rv::app::walkerActionsFor(WorldContext::Edit, walking);
    CHECK(walked.axis(Action::MoveForward) == 0.0f);
    CHECK(walked.axis(Action::LookX) == 0.5f);

    // The interaction enters Edit from free roam, Focus or Inspect, never while launching, and leaves it for free roam.
    rv::app::WorldInteraction interaction;
    CHECK(interaction.enterEdit());
    CHECK(interaction.state() == rv::app::WorldState::Edit);
    CHECK(interaction.context() == WorldContext::Edit);
    CHECK(interaction.backLevel() == rv::app::WorldBack::Shell);
    CHECK_FALSE(interaction.enterEdit());
    const ActionState interact = press({Action::Interact, Action::QuickCatalog});
    rv::app::InteractionFrame input;
    input.actions = &interact;
    const rv::app::InteractionResult result = interaction.update(input);
    CHECK_FALSE(result.openCatalog);
    CHECK(interaction.state() == rv::app::WorldState::Edit);
    interaction.leaveEdit();
    CHECK(interaction.state() == rv::app::WorldState::FreeRoam);
    CHECK(rv::app::toString(rv::app::CardAction::Move) == "move");
}

TEST_CASE("app/world_edit: the editor picks up, moves, swaps, pins, and puts back", "[app][world_edit]") {
    const std::unique_ptr<EditRoom> room = editRoomOf(room80().plan);
    const auto [item, free] = itemBesideFree(*room);
    const GameId game = room->target(item)->game.value();
    EditInteraction editor({}, true);
    editor.begin(std::nullopt);
    editor.setRoom(room.get());
    CHECK(editor.mode() == EditMode::Preparing);
    // Nothing is read while the room's data is read.
    CHECK_FALSE(pressOn(editor, {Action::Interact}, item).apply);
    editor.ready();
    REQUIRE(editor.mode() == EditMode::Browse);

    // Picked up, put on a free slot: a move.
    aim(editor, item);
    static_cast<void>(pressOn(editor, {Action::Interact}, item));
    REQUIRE(editor.mode() == EditMode::Holding);
    CHECK(editor.held() == game);
    CHECK(editor.heldFrom() == item);
    aim(editor, free);
    EditFrameResult result = pressOn(editor, {Action::Interact}, free);
    REQUIRE(result.apply.has_value());
    CHECK(*result.apply == EditOperation::move(game, room->target(free)->slot));
    CHECK(editor.mode() == EditMode::Busy);
    // Input waits while the operation is applied.
    CHECK_FALSE(pressOn(editor, {Action::Interact}, free).apply);
    editor.applied(true);
    CHECK(editor.mode() == EditMode::Browse);
    CHECK_FALSE(editor.held().has_value());

    // Two games swapped; a refusal keeps the game in the hand.
    const rv::world::CollectionItem& first = room->layout().items.front();
    const rv::world::CollectionItem& second = room->layout().items.back();
    aim(editor, first.pickId);
    static_cast<void>(pressOn(editor, {Action::Interact}, first.pickId));
    aim(editor, second.pickId);
    result = pressOn(editor, {Action::Interact}, second.pickId);
    REQUIRE(result.apply.has_value());
    CHECK(*result.apply == EditOperation::swap(first.game, second.game));
    editor.applied(false);
    CHECK(editor.mode() == EditMode::Holding);
    CHECK(editor.held() == first.game);
    // Back puts it back; on its own slot interact puts it back too.
    static_cast<void>(pressOn(editor, {Action::Back}, second.pickId));
    CHECK(editor.mode() == EditMode::Browse);
    CHECK_FALSE(editor.held().has_value());
    aim(editor, first.pickId);
    static_cast<void>(pressOn(editor, {Action::Interact}, first.pickId));
    REQUIRE(editor.mode() == EditMode::Holding);
    result = pressOn(editor, {Action::Interact}, first.pickId);
    CHECK_FALSE(result.apply.has_value());
    CHECK(editor.mode() == EditMode::Browse);

    // edit_pin pins an unpinned game, unpins a pinned one.
    bool pinned = false;
    editor.setPinned([&](const GameId&) { return pinned; });
    result = pressOn(editor, {Action::EditPin}, first.pickId);
    REQUIRE(result.apply.has_value());
    CHECK(*result.apply == EditOperation::pin(first.game));
    editor.applied(true);
    pinned = true;
    CHECK(editor.targetPinned());
    result = pressOn(editor, {Action::EditPin}, first.pickId);
    REQUIRE(result.apply.has_value());
    CHECK(*result.apply == EditOperation::unpin(first.game));
    editor.applied(true);
    // A free slot can be neither picked up nor pinned.
    aim(editor, free);
    CHECK_FALSE(pressOn(editor, {Action::EditPin}, free).apply);
    static_cast<void>(pressOn(editor, {Action::Interact}, free));
    CHECK(editor.mode() == EditMode::Browse);
}

TEST_CASE("app/world_edit: the D-pad steps over every slot, empty ones included, and the reticle takes back",
          "[app][world_edit]") {
    const std::unique_ptr<EditRoom> room = editRoomOf(room80().plan);
    const auto [item, free] = itemBesideFree(*room);
    EditInteraction editor({}, true);
    editor.begin(std::nullopt);
    editor.setRoom(room.get());
    editor.ready();
    aim(editor, item);
    const EditFrameResult stepped = pressOn(editor, {Action::UiRight}, item);
    REQUIRE(editor.target().has_value());
    CHECK(editor.target()->pick == free);
    CHECK(editor.target()->free());
    // The walker stands in front of it (instant under reduceMotion).
    REQUIRE(stepped.walker.has_value());
    // The step's target holds while the reticle rests elsewhere, until the player looks.
    static_cast<void>(frame(editor, nullptr, item));
    CHECK(editor.target()->pick == free);
    ActionState look;
    look.setAxis(Action::LookDeltaX, 3.0f);
    for (int i = 0; i < rv::world::kHoverChangeFrames + 1; ++i) {
        static_cast<void>(frame(editor, &look, item));
    }
    CHECK(editor.target()->pick == item);

    // An arrow key is bound to a step and to walking (UiRight and MoveRight at once): it steps, and its own walk axis
    // does not give the step's target back to the reticle (the walker does not walk while it is held).
    ActionState arrow = press({Action::UiRight});
    arrow.setAxis(Action::MoveRight, 1.0f);
    static_cast<void>(frame(editor, &arrow, item));
    REQUIRE(editor.target().has_value());
    CHECK(editor.target()->pick == free);
    ActionState held;
    held.setButton(Action::UiRight, true, false, false);
    held.setAxis(Action::MoveRight, 1.0f);
    for (int i = 0; i < rv::world::kHoverChangeFrames + 1; ++i) {
        static_cast<void>(frame(editor, &held, item));
    }
    CHECK(editor.target()->pick == free);

    // With the eye outside the edited room the D-pad steps nothing (the app samples no target there either).
    EditFrame outside;
    const ActionState left = press({Action::UiLeft});
    outside.actions = &left;
    outside.deltaSeconds = 0.016667f;
    outside.inRoom = false;
    static_cast<void>(editor.update(outside));
    CHECK(editor.target()->pick == free);
}

TEST_CASE("app/world_edit: the menu, Back's levels, \"Sposta\" with the item held", "[app][world_edit]") {
    const std::unique_ptr<EditRoom> room = editRoomOf(room80().plan);
    const auto [item, free] = itemBesideFree(*room);
    const GameId game = room->target(item)->game.value();
    EditInteraction editor({}, true);
    // "Sposta": the item in the hand once ready.
    editor.begin(game);
    editor.setRoom(room.get());
    editor.ready();
    CHECK(editor.mode() == EditMode::Holding);
    CHECK(editor.held() == game);
    CHECK(editor.heldFrom() == item);

    // The menu (Start, Enter) from the hand puts the game back; Back or Continua leaves it.
    static_cast<void>(pressOn(editor, {Action::Confirm}, free));
    REQUIRE(editor.mode() == EditMode::Menu);
    CHECK_FALSE(editor.held().has_value());
    CHECK(editor.menuFocus() == EditMenuButton::Save);
    static_cast<void>(pressOn(editor, {Action::Back}, free));
    CHECK(editor.mode() == EditMode::Browse);
    static_cast<void>(pressOn(editor, {Action::Confirm}, free));
    static_cast<void>(pressOn(editor, {Action::UiDown}, free));
    static_cast<void>(pressOn(editor, {Action::UiDown}, free));
    static_cast<void>(pressOn(editor, {Action::UiDown}, free));
    static_cast<void>(pressOn(editor, {Action::UiDown}, free)); // stays on the last
    CHECK(editor.menuFocus() == EditMenuButton::Continue);
    static_cast<void>(pressOn(editor, {Action::UiActivate}, free));
    CHECK(editor.mode() == EditMode::Browse);

    // Nothing pending: Back leaves the editor, Salva and Esci senza salvare too, Annulla does nothing.
    EditFrameResult result = pressOn(editor, {Action::Back}, free);
    CHECK(result.exit);
    static_cast<void>(pressOn(editor, {Action::Confirm}, free));
    static_cast<void>(pressOn(editor, {Action::UiDown}, free));
    result = pressOn(editor, {Action::UiActivate}, free);
    CHECK_FALSE(result.undo);
    CHECK(editor.mode() == EditMode::Menu);
    static_cast<void>(pressOn(editor, {Action::UiUp}, free));
    result = pressOn(editor, {Action::UiActivate}, free);
    CHECK(result.exit);
    CHECK_FALSE(result.save);
    static_cast<void>(pressOn(editor, {Action::Back}, free)); // Continua
    CHECK(editor.mode() == EditMode::Browse);

    // Pending edits: Back asks (the menu); Annulla ultima azione undoes one; Salva saves; Esci senza salvare discards.
    editor.setPending(2);
    static_cast<void>(pressOn(editor, {Action::Back}, free));
    REQUIRE(editor.mode() == EditMode::Menu);
    static_cast<void>(pressOn(editor, {Action::UiDown}, free));
    CHECK(editor.menuFocus() == EditMenuButton::Undo);
    result = pressOn(editor, {Action::Interact}, free);
    CHECK(result.undo);
    CHECK(editor.mode() == EditMode::Busy);
    editor.undone();
    CHECK(editor.mode() == EditMode::Browse);
    static_cast<void>(pressOn(editor, {Action::Confirm}, free));
    // The pad's Start (confirm alone) closes the menu: Continua, never Salva on a second press.
    result = pressOn(editor, {Action::Confirm}, free);
    CHECK_FALSE(result.save);
    CHECK(editor.mode() == EditMode::Browse);
    static_cast<void>(pressOn(editor, {Action::Confirm}, free));
    REQUIRE(editor.mode() == EditMode::Menu);
    result = pressOn(editor, {Action::Confirm, Action::UiActivate}, free); // Enter presses the focused Salva
    CHECK(result.save);
    CHECK(editor.mode() == EditMode::Saving);
    CHECK_FALSE(pressOn(editor, {Action::Back}, free).exit); // the save runs: input waits
    editor.saved(false);
    CHECK(editor.mode() == EditMode::Browse); // refused: the session stays open
    static_cast<void>(pressOn(editor, {Action::Confirm}, free));
    static_cast<void>(pressOn(editor, {Action::UiDown}, free));
    static_cast<void>(pressOn(editor, {Action::UiDown}, free));
    CHECK(editor.menuFocus() == EditMenuButton::Discard);
    result = pressOn(editor, {Action::UiActivate}, free);
    CHECK(result.discard);
    CHECK(editor.mode() == EditMode::Saving);
    editor.saved(true);
    CHECK(editor.mode() == EditMode::Preparing);

    // The shell's levels come first: a popup closes, the editor stays.
    editor.end();
    editor.begin(std::nullopt);
    editor.setRoom(room.get());
    editor.ready();
    EditFrame input;
    const ActionState back = press({Action::Back});
    input.actions = &back;
    input.back.overlayVisible = true;
    input.back.overlayPopupOpen = true;
    result = editor.update(input);
    REQUIRE(result.shellBack.has_value());
    CHECK(*result.shellBack == rv::app::BackAction::ClosePopup);
    CHECK_FALSE(result.exit);
    CHECK(editor.mode() == EditMode::Browse);
    // The editor's own level comes before the shell's Quit.
    input.back = {};
    result = editor.update(input);
    CHECK_FALSE(result.shellBack.has_value());
    CHECK(result.exit);
}

TEST_CASE("app/world_edit: the draft keeps each operation and undoes it back to the base", "[app][world_edit]") {
    const auto data = snesEditData();
    const EditedWorld base{room80().plan, {}};
    EditDraft draft(rv::app::EditBase{base, "base"}, data);
    CHECK_FALSE(draft.pending());
    const std::unique_ptr<EditRoom> room = editRoomOf(room80().plan);
    const auto [item, free] = itemBesideFree(*room);
    const GameId game = room->target(item)->game.value();
    const SlotId to = room->target(free)->slot;

    auto moved = draft.apply(EditOperation::move(game, to), kProvenance);
    REQUIRE(moved.has_value());
    REQUIRE_FALSE(moved->refused);
    CHECK(draft.size() == 1);
    CHECK(placementOf(draft.current().plan, game)->slot == to);
    CHECK(draft.pinned(game));
    const EditedWorld afterMove = draft.current();
    // A refusal is not kept; neither is a pin that is there already.
    auto refused = draft.apply(EditOperation::move(room->layout().items.back().game, to), kProvenance);
    REQUIRE(refused.has_value());
    CHECK(refused->refused.has_value());
    CHECK(refused->refused->rule == rv::world::edit_rule::kSlotTaken);
    auto again = draft.apply(EditOperation::pin(game), kProvenance);
    REQUIRE(again.has_value());
    CHECK_FALSE(again->changed);
    CHECK(draft.size() == 1);
    CHECK(draft.current() == afterMove);
    // A swap, then two undos: the base byte for byte.
    const GameId a = room->layout().items.front().game;
    const GameId b = room->layout().items.back().game;
    auto swapped = draft.apply(EditOperation::swap(a, b), kProvenance);
    REQUIRE(swapped.has_value());
    REQUIRE_FALSE(swapped->refused);
    CHECK(draft.size() == 2);
    CHECK(draft.operations().back() == EditOperation::swap(a, b));
    // What an undo shows before the draft takes it back (the app's job), each time the draft as undo() leaves it.
    CHECK(draft.previous() == afterMove);
    CHECK(draft.undo());
    CHECK(draft.previous() == base);
    CHECK(draft.current() == afterMove);
    CHECK(draft.undo());
    CHECK(draft.current() == base);
    CHECK_FALSE(draft.pinned(game));
    CHECK_FALSE(draft.undo());
    // An operation applied elsewhere (the app's job) is adopted as the draft's last.
    EditedWorld copy = draft.current();
    auto applied = rv::world::applyEdit(copy, EditOperation::move(game, to), *data, kProvenance);
    REQUIRE(applied.has_value());
    draft.adopt(EditOperation::move(game, to), *applied, copy);
    CHECK(draft.current() == afterMove);
}

TEST_CASE("app/world_edit: a save writes the draft behind a snapshot that undo restores", "[app][world_edit]") {
    const SavedWorld saved;
    const std::string before = saved.planDigest();
    const std::unique_ptr<EditRoom> room = editRoomOf(room80().plan);
    const auto [item, free] = itemBesideFree(*room);
    const GameId game = room->target(item)->game.value();
    EditDraft draft(saved.base(), snesEditData());
    REQUIRE(draft.apply(EditOperation::move(game, room->target(free)->slot), kProvenance).has_value());
    REQUIRE(draft.apply(EditOperation::swap(room->layout().items.front().game, room->layout().items.back().game),
                        kProvenance)
                .has_value());
    REQUIRE(draft.size() == 2);

    auto outcome = rv::app::saveDraft(draft, saved.saveOptions());
    REQUIRE(outcome.has_value());
    CHECK(outcome->written);
    CHECK_FALSE(outcome->replayed);
    CHECK_FALSE(outcome->refused.has_value());
    CHECK_FALSE(outcome->snapshot.empty());
    CHECK(outcome->snapshot.ends_with("-edit"));
    CHECK(outcome->saved.plan == draft.current().plan);
    CHECK(outcome->planDigest == saved.planDigest());
    CHECK(outcome->planDigest != before);
    // The plan and the overrides on disk are the draft's.
    auto stored = rv::world::readStoredPlan(saved.files.plan);
    REQUIRE(stored.has_value());
    CHECK(stored->plan == draft.current().plan);
    auto overrides = rv::world::loadOverrides(saved.files.overrides);
    REQUIRE(overrides.has_value());
    CHECK(*overrides == draft.current().overrides);
    CHECK(overrides->gamePins.size() == 3);
    // Saving the same again writes nothing.
    EditDraft same(saved.base(), snesEditData());
    auto nothing = rv::app::saveDraft(same, saved.saveOptions());
    REQUIRE(nothing.has_value());
    CHECK_FALSE(nothing->written);
    // "Annulla ultima modifica": the plan before the session, byte for byte.
    auto lock = rv::world::WorldLock::acquire(saved.files);
    REQUIRE(lock.has_value());
    auto undone = rv::world::undoLastAction(*lock);
    REQUIRE(undone.has_value());
    CHECK(undone->restored.action == "edit");
    CHECK(saved.planDigest() == before);
}

TEST_CASE("app/world_edit: a save replayed over a plan changed meanwhile, applied or refused with nothing written",
          "[app][world_edit]") {
    const std::unique_ptr<EditRoom> room = editRoomOf(room80().plan);
    const auto [item, free] = itemBesideFree(*room);
    const GameId game = room->target(item)->game.value();
    const SlotId to = room->target(free)->slot;
    // Two other games of the room.
    std::vector<GameId> others;
    for (const rv::world::CollectionItem& entry : room->layout().items) {
        if (entry.game != game) {
            others.push_back(entry.game);
        }
    }
    REQUIRE(others.size() >= 2);
    const GameId a = others.front();
    const GameId b = others.back();

    // Another writer (the command line) edits the stored world after the session began.
    const auto otherEdit = [&](const SavedWorld& saved, const EditOperation& operation) {
        auto lock = rv::world::WorldLock::acquire(saved.files);
        REQUIRE(lock.has_value());
        auto stored = rv::world::readStoredPlan(saved.files.plan);
        REQUIRE(stored.has_value());
        EditedWorld world{stored->plan, {}};
        auto result = rv::world::applyEdit(world, operation, *snesEditData(),
                                           rv::world::PinProvenance{rv::world::PinOrigin::Cli, ""});
        REQUIRE(result.has_value());
        REQUIRE_FALSE(result->refused);
        auto edited = rv::world::editedResult(stored->plan, stored->digest, world, std::vector{*result});
        REQUIRE(edited.has_value());
        REQUIRE(rv::world::commitWorld(*lock, rv::world::WorldAction{"edit", "room:snes"}, *edited, world.overrides)
                    .has_value());
    };

    SECTION("the session's operations still apply: replayed, written") {
        const SavedWorld saved;
        EditDraft draft(saved.base(), snesEditData());
        REQUIRE(draft.apply(EditOperation::move(game, to), kProvenance).has_value());
        otherEdit(saved, EditOperation::swap(a, b));
        auto outcome = rv::app::saveDraft(draft, saved.saveOptions());
        REQUIRE(outcome.has_value());
        CHECK(outcome->replayed);
        CHECK(outcome->written);
        CHECK_FALSE(outcome->refused.has_value());
        // Both edits stand: the other writer's swap and the session's move.
        auto stored = rv::world::readStoredPlan(saved.files.plan);
        REQUIRE(stored.has_value());
        CHECK(placementOf(stored->plan, game)->slot == to);
        CHECK(placementOf(stored->plan, a)->slot == placementOf(room80().plan, b)->slot);
        CHECK(placementOf(stored->plan, b)->slot == placementOf(room80().plan, a)->slot);
        CHECK(outcome->saved.plan == stored->plan);
        CHECK(outcome->planDigest == stored->digest);
    }
    SECTION("an operation no longer applies: refused with its rule, nothing written") {
        const SavedWorld saved;
        EditDraft draft(saved.base(), snesEditData());
        REQUIRE(draft.apply(EditOperation::move(game, to), kProvenance).has_value());
        // The other writer put another game into the session's free slot.
        otherEdit(saved, EditOperation::move(b, to));
        const std::string changed = saved.planDigest();
        auto outcome = rv::app::saveDraft(draft, saved.saveOptions());
        REQUIRE(outcome.has_value());
        CHECK(outcome->replayed);
        CHECK_FALSE(outcome->written);
        REQUIRE(outcome->refusedAt.has_value());
        CHECK(*outcome->refusedAt == 0);
        REQUIRE(outcome->refused.has_value());
        CHECK(outcome->refused->rule == rv::world::edit_rule::kSlotTaken);
        CHECK(saved.planDigest() == changed);
        CHECK(outcome->planDigest == changed);
    }
}
