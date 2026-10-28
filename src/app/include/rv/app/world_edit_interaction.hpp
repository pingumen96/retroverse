#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The editor's interaction (I4b step 3; ADR-0015 §4, amending ADR-0012 §1): the state Edit's own modes and input,
// driven by the frame's actions in WorldContext::Edit and the frame's pick sample over the edited room (EditRoom),
// tested without a window. Nothing here edits a plan: the results ask the session (app::WorldSession, world_edit.cpp)
// to apply an operation to the draft (world_edit_draft.hpp), to undo the draft's last, to save, to discard or to leave.
//
// | Mode      | What the player does                                                   | Leaves                        |
// |-----------|------------------------------------------------------------------------|-------------------------------|
// | Preparing | (the session reads the world's data for the room: nothing to do)       | ready(): Browse, or Holding   |
// |           |                                                                        | ("Sposta": the item held)     |
// | Browse    | walks and looks; the reticle (or the D-pad's steps over every slot,    | interact on a game: Holding;  |
// |           | empty ones included) targets a game or an empty slot; edit_pin pins or | confirm: Menu; Back: Menu     |
// |           | unpins the targeted game in place                                      | with pending edits, else Exit |
// | Holding   | the game held (the app draws it in the hand, smaller); interact on an  | interact: an operation (Busy) |
// |           | empty slot moves it there, on another game swaps the two, on its own   | or Browse; Back: Browse (put  |
// |           | slot puts it back                                                      | back); confirm: Menu          |
// | Busy      | an operation is applied to the draft and shown (input waits)           | applied(): Browse (held on a  |
// |           |                                                                        | refusal, the bar naming it)   |
// | Menu      | Salva, Annulla ultima azione, Esci senza salvare, Continua: Up and     | the press's command; Back,    |
// |           | Down move the focus, interact or ui_activate (Enter) presses           | Start or Continua: Browse     |
// | Saving    | the draft saved (input waits)                                          | saved(): Exit, or Browse with |
// |           |                                                                        | the refusal's reasons         |
// | Moving    | (I4b step 4) a fixture held: it follows the reticle's floor point (a   | interact: an operation (Busy) |
// |           | hung decoration its wall point) on the room's grid (setPlacer()),      | or Browse (put back where it  |
// |           | edit_rotate_left/right turn it (a quarter turn, decoration 15          | stood); Back: Browse; confirm:|
// |           | degrees), the D-pad nudges it by a grid step along the room's axis     | Menu                          |
// |           | nearest the view's (a hung decoration along its wall) until the player |                               |
// |           | looks or walks; each new place is judged (probe) and its verdict shown |                               |
//
// In Browse, interact on a movable fixture (the reticle on it where no game or slot is: EditFrame::fixture) picks it up
// (Moving), unless it cannot move (a room kept as an older template made it: the refusal named, nothing held). Where it
// is held from the reticle's floor point is kept (its grab offset), so that it does not jump to the reticle.
// The hover over the room's targets follows world::HoverTracker's stability rules (ADR-0012 §2); a D-pad step targets
// its slot exactly and eases the walker to its pose (InteractionTimings::focusEase, instant under reduceMotion), until
// the player looks or walks again. Back takes the shell's levels first (worldBackAction()): a popup, a panel; the
// editor's own level always comes before the shell's ReleaseMouse, CloseOverlay and Quit.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <glm/glm.hpp>

#include <rv/app/back_action.hpp>
#include <rv/app/world_edit_room.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/app/world_interaction.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/pose_animator.hpp>
#include <rv/scene/viewpoint.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/world_edit.hpp>

namespace rv::app {

enum class EditMode : std::uint8_t { Preparing, Browse, Holding, Busy, Menu, Saving, Moving };

/// "preparing", "browse", "holding", "busy", "menu", "saving", "moving".
[[nodiscard]] std::string_view toString(EditMode mode) noexcept;

/// The menu's buttons, top to bottom (ADR-0015 §4).
enum class EditMenuButton : std::uint8_t { Save, Undo, Discard, Continue };

/// "save", "undo", "discard", "continue".
[[nodiscard]] std::string_view toString(EditMenuButton button) noexcept;

/// One frame as the editor sees it.
struct EditFrame {
    /// The frame's actions as resolved (before any context), or nullptr while the product UI has the input.
    const input::ActionState* actions = nullptr;
    float deltaSeconds = 0.0f;
    /// The walker's pose as the frame starts.
    scene::WalkerPose walker;
    /// The frame's ray against EditRoom::index() with hovered() as the current hover; nothing without a ray or room.
    std::optional<world::HoverSample> hover;
    /// The shell's Back levels.
    BackState back;
    /// Where a step's pose is framed from.
    FocusViewer viewer;
    /// The eye is in the edited room: outside it the D-pad steps nothing (the app samples no target there either).
    bool inRoom = true;
    /// I4b step 4: the movable fixture the ray reaches where no game or slot is (EditRoom::fixtureAt()), if any.
    std::optional<world::FixtureId> fixture;
    /// Where the reticle aims, in the room's frame (millimetres; the yaw unused): the floor under it (EditRoom::
    /// floorAim()) and the wall (wallAim()); nothing when it meets neither.
    std::optional<world::FixtureTransform> floorAim;
    std::optional<world::FixtureTransform> wallAim;
    /// The view's forward on the floor in the room's frame (x, z; EditRoom::roomDirection()), for the D-pad's nudges.
    glm::vec2 roomForward{0.0f, -1.0f};
};

/// A fixture held (Moving): where it stood, where it would stand now (snapped by the placer), how it is held.
struct HeldFixture {
    world::FixtureId id;
    world::FixtureRole role = world::FixtureRole::Decoration;
    /// A decoration hung on a wall: it slides along the walls only, the wall turning it.
    bool hung = false;
    world::FixtureTransform from;
    world::FixtureTransform place;
    /// The pivot's offset from the floor point under the reticle when it was picked up (a hung decoration's: from the
    /// wall point; millimetres, room's frame).
    std::int64_t offsetXMm = 0;
    std::int64_t offsetZMm = 0;
    /// The yaw asked (the turns so far), which the placer rounds to the fixture's step.
    std::int32_t yawCdeg = 0;

    /// It stands elsewhere than where it was picked up.
    [[nodiscard]] bool moved() const noexcept { return place != from; }
};

/// Snaps a wanted place of a fixture to the nearest its edit's own rules accept (world::snapFixture() on the draft);
/// nothing when the fixture cannot move.
using FixturePlacer =
    std::function<std::optional<world::FixtureTransform>(const world::FixtureId&, const world::FixtureTransform&)>;

/// What the session does after a frame.
struct EditFrameResult {
    /// Back taken by a level of the shell: the application performs it.
    std::optional<BackAction> shellBack;
    /// An operation for the draft (Busy until applied()).
    std::optional<world::EditOperation> apply;
    /// The menu's commands: the draft's last operation undone; the draft saved (Saving until saved()); the session
    /// left without saving (the plan shown as stored); the session left with nothing pending (Browse's Back, Salva or
    /// Esci senza salvare with no edit).
    bool undo = false;
    bool save = false;
    bool discard = false;
    bool exit = false;
    /// The walker's pose this frame while a step's ease drives it.
    std::optional<scene::WalkerPose> walker;
    /// I4b step 4: an operation to judge ahead on a copy of the draft (a held fixture's new place: setVerdict() then
    /// says what it found; a held game's move or swap onto the slot targeted: its drop there shows what the probe
    /// made); the rule a fixture the player tried to pick up is kept by ("": none).
    std::optional<world::EditOperation> probe;
    std::string refused;
};

class EditInteraction {
public:
    explicit EditInteraction(InteractionTimings timings = {}, bool reduceMotion = false);

    /// A session begins (Preparing): `held`, the game "Sposta" picked up (held once ready()).
    void begin(std::optional<catalog::GameId> held);
    /// The room's data is read and shown: Browse, or Holding the game begin() named (Browse when the room lacks it).
    void ready();
    /// The edited room (it must outlive its use; nullptr: none), set again after every shown edit: the target and the
    /// held game are kept by game and slot, the hover by its pick id when the room still has it.
    void setRoom(const EditRoom* room);
    /// How many operations the draft holds (Back with pending edits asks: the menu), and whether a game is pinned (the
    /// overrides of the draft: edit_pin pins an unpinned game, unpins a pinned one).
    void setPending(std::size_t operations) noexcept { m_pending = operations; }
    void setPinned(std::function<bool(const catalog::GameId&)> pinned) { m_pinned = std::move(pinned); }
    /// I4b step 4: how a held fixture's wanted place is snapped (world::snapFixture() on the draft), and whether a
    /// fixture is pinned (the bar's "Fissato").
    void setPlacer(FixturePlacer placer) { m_placer = std::move(placer); }
    void setFixturePinned(std::function<bool(const world::FixtureId&)> pinned) { m_fixturePinned = std::move(pinned); }
    /// The verdict on a held fixture's place (`refusal`: the rule that refuses it, "" none), from the probe the frame
    /// asked: kept when it is about the fixture held and the place it stands at now.
    void setVerdict(const world::EditOperation& probe, std::string refusal);

    /// One frame: Back, then the mode's actions, then the hover and the step's ease.
    [[nodiscard]] EditFrameResult update(const EditFrame& frame);

    /// The operation of Busy was applied (`accepted`: the draft took it) or refused: Browse, the game still held after a
    /// refused move or swap.
    void applied(bool accepted);
    /// The draft's last operation was undone (or there was none): Browse.
    void undone();
    /// The save of Saving ended: written (the session ends: nothing more to do) or refused (Browse: the menu's reasons
    /// shown by the bar).
    void saved(bool written);
    /// The session ended (or never began): back to Preparing, nothing held.
    void end();
    void setReduceMotion(bool on);

    [[nodiscard]] EditMode mode() const noexcept { return m_mode; }
    /// The pick id the reticle rests on (world::HoverTracker's), for the frame's sample.
    [[nodiscard]] world::PickId hovered() const noexcept { return m_hover.hovered(); }
    /// What the reticle or the last step rests on, if anything.
    [[nodiscard]] const std::optional<EditTarget>& target() const noexcept { return m_target; }
    /// The game held (Holding, and Busy over a move or a swap), and the pick id of its slot when it was picked up.
    [[nodiscard]] const std::optional<catalog::GameId>& held() const noexcept { return m_held; }
    [[nodiscard]] world::PickId heldFrom() const noexcept { return m_heldFrom; }
    [[nodiscard]] EditMenuButton menuFocus() const noexcept { return m_menuFocus; }
    /// The game held is pinned now (for the bar).
    [[nodiscard]] bool targetPinned() const;
    /// I4b step 4: the fixture held (Moving, and Busy over its drop).
    [[nodiscard]] const std::optional<HeldFixture>& heldFixture() const noexcept { return m_fixture; }
    /// The verdict on where the held fixture stands now: nothing while it is judged, "" when it may drop there, else
    /// the rule that refuses it.
    [[nodiscard]] std::optional<std::string> verdict() const;

private:
    void readBrowse(const input::ActionState& actions, const EditFrame& frame, EditFrameResult& result);
    void readHolding(const input::ActionState& actions, EditFrameResult& result);
    void readMenu(const input::ActionState& actions, EditFrameResult& result);
    /// I4b step 4: Browse's pick-up of a fixture; Moving's turns, nudges, aim, drop and probe.
    bool pickUpFixture(const input::ActionState& actions, const EditFrame& frame, EditFrameResult& result);
    void readMoving(const input::ActionState& actions, const EditFrame& frame, bool looked, EditFrameResult& result);
    /// The fixture held placed where `wanted` snaps to (nothing changes when the placer refuses it).
    void placeFixture(const world::FixtureTransform& wanted);
    void back(const BackState& shell, EditFrameResult& result);
    void openMenu();
    void step(const input::ActionState& actions, const EditFrame& frame);
    void track(const EditFrame& frame, bool looked);
    void retarget(world::PickId pick);

    InteractionTimings m_timings;
    bool m_reduceMotion = false;
    EditMode m_mode = EditMode::Preparing;
    const EditRoom* m_room = nullptr;
    std::size_t m_pending = 0;
    std::function<bool(const catalog::GameId&)> m_pinned;
    world::HoverTracker m_hover;
    std::optional<EditTarget> m_target;
    /// The target comes from a D-pad step (kept until the player looks or walks).
    bool m_stepped = false;
    std::optional<catalog::GameId> m_held;
    world::PickId m_heldFrom = world::kNoPick;
    /// The game "Sposta" asked to hold once ready().
    std::optional<catalog::GameId> m_heldAtStart;
    EditMenuButton m_menuFocus = EditMenuButton::Save;
    scene::PoseAnimator<scene::WalkerPose> m_walker;
    scene::WalkerPose m_walkerNow;
    bool m_walkerPlaced = false;
    /// I4b step 4: the fixture held, its placer, the place last asked to be judged and the verdict on a place.
    std::optional<HeldFixture> m_fixture;
    FixturePlacer m_placer;
    std::function<bool(const world::FixtureId&)> m_fixturePinned;
    std::optional<world::FixtureTransform> m_probed;
    std::optional<std::pair<world::FixtureTransform, std::string>> m_verdict;
    /// The held fixture was nudged by the D-pad: the reticle takes it again once the player looks or walks.
    bool m_nudged = false;
    /// The slot a held game's probe was asked for last.
    world::PickId m_probedPick = world::kNoPick;
};

} // namespace rv::app
