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
// | Menu      | Salva, Annulla ultima azione, Esci senza salvare, Continua: Up and     | the press's command; Back or  |
// |           | Down move the focus, interact, ui_activate or confirm presses          | Continua: Browse              |
// | Saving    | the draft saved (input waits)                                          | saved(): Exit, or Browse with |
// |           |                                                                        | the refusal's reasons         |
//
// The hover over the room's targets follows world::HoverTracker's stability rules (ADR-0012 §2); a D-pad step targets
// its slot exactly and eases the walker to its pose (InteractionTimings::focusEase, instant under reduceMotion), until
// the player looks or walks again. Back takes the shell's levels first (worldBackAction()): a popup, a panel; the
// editor's own level always comes before the shell's ReleaseMouse, CloseOverlay and Quit.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

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

enum class EditMode : std::uint8_t { Preparing, Browse, Holding, Busy, Menu, Saving };

/// "preparing", "browse", "holding", "busy", "menu", "saving".
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
};

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

private:
    void readBrowse(const input::ActionState& actions, EditFrameResult& result);
    void readHolding(const input::ActionState& actions, EditFrameResult& result);
    void readMenu(const input::ActionState& actions, EditFrameResult& result);
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
};

} // namespace rv::app
