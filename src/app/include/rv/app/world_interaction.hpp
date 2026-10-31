#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// The interaction with the generated room (ADR-0012 §1-§4, §6, §8; I3 step 7): the states FreeRoam, Focus, Inspect,
// Launching and Returning, their transitions and failure paths, each state's input context and Back levels, the hover
// with its pull-out after a dwell, the walker's eases, the lift of an inspected item, the left-click rule and the pick
// ray's source. Everything is driven by the frame's actions and frame time (the fixed one under --fixed-dt), so a
// scripted run interacts identically every time; nothing here draws, picks or launches: the app (I3 steps 13a, 13b,
// 14a, 15a) feeds it the frame's pick sample and does what its results ask.
//
// | State     | Enter                                                    | Leave                                         |
// |-----------|----------------------------------------------------------|-----------------------------------------------|
// | FreeRoam  | the start; Back from Focus                               | interact on a hovered item: Focus (spine-out) |
// |           |                                                          | or Inspect (face-out)                         |
// | Focus     | interact on a spine-out item; Back from Inspect;         | interact or ui_activate: Inspect; Back:       |
// |           | showInRoom()                                             | FreeRoam, the item put back                   |
// | Inspect   | interact or ui_activate in Focus; interact on a face-out | Avvia: AnimatingToConsole in a room with a    |
// |           | item in FreeRoam; the return of a launch                 | console, else Launching; Back: Focus, the     |
// |           |                                                          | item lowered to its pulled-out pose           |
// | Animating | Avvia in a room with a console (I5 step 9, ADR-0016      | the insertion's end (450 ms, or any button:   |
// | ToConsole | §11), not under reduceMotion                             | the skip): Launching; launchEnded() (refused  |
// |           |                                                          | meanwhile): Returning                         |
// | Launching | Avvia (straight from Inspect without a console or under  | launchEnded(): Returning                      |
// |           | reduceMotion), the insertion's end                       |                                               |
// | Returning | launchEnded()                                            | the next update: Inspect of the same item     |
// |           |                                                          | (FreeRoam when it left the room)              |
// | Edit      | enterEdit() (I4b step 3: the directory's "Modifica       | leaveEdit(): FreeRoam                         |
// |           | stanza", the card's "Sposta")                            |                                               |
//
// In Edit (ADR-0015 §4) the interaction reads nothing and drives nothing: the editor (world_edit_interaction.hpp) has
// the frame's actions in the Edit context, the walker walks and looks as in FreeRoam.
//
// A failed transition (an item the room no longer has, a refused launch) leaves a usable state and a notice() for the
// card. The timings (ADR-0012 §3, §4, §8) are InteractionTimings; each transition the player starts is skippable with
// the button that started it (interact or ui_activate for the ease into Focus and for the lift, Back for the lowering
// back to Focus), and every animation is instant under reduceMotion (the hover's dwell still applies).

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <rv/app/back_action.hpp>
#include <rv/app/launch_outcome.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/pose_animator.hpp>
#include <rv/scene/viewpoint.hpp>
#include <rv/world/pick_index.hpp>
#include <rv/world/slot_navigation.hpp>

namespace rv::app {

/// The world's interaction states (ADR-0012 §1; specification §8.3's OnShelf, Focused, HeldForInspection,
/// AnimatingToConsole, Launching, Returning).
enum class WorldState : std::uint8_t { FreeRoam, Focus, Inspect, Launching, Returning, Edit, AnimatingToConsole };

/// "free roam", "focus", "inspect", "launching", "returning", "edit", "animating to console".
[[nodiscard]] std::string_view toString(WorldState state) noexcept;

/// Who reads the world's actions in each state, inside the action map's World context (input_context.hpp: the product
/// UI's Ui context still takes everything while the quick catalog or the game-session panel is open). The contexts
/// keep one meaning per button (specification §12.2):
///  - FreeRoam: the World context's actions (walking, looking, interact, Back, QuickCatalog), no UI action.
///  - Focus: the look actions (they move the pick within the shelf section), ui_up / ui_down / ui_left / ui_right (the
///    arrows and the D-pad step between items), interact and ui_activate (inspect), Back, QuickCatalog. No walking.
///  - Inspect: the look actions (they rotate the item), ui_page_up / ui_page_down (the triggers and Page Up / Down
///    zoom; the wheel too, InteractionFrame::wheelNotches), the card's actions: confirm without ui_activate (the pad's
///    Start: Avvia), ui_media (Vedi media), ui_info (Informazioni), ui_back (Indietro, Back's levels), the arrows and
///    ui_activate (its buttons' focus and the press of the focused one, I3 step 14a: InteractionFrame::card, so Enter,
///    which is both confirm and ui_activate, presses the focused button), interact and ui_activate (skip the lift).
///    Neither QuickCatalog nor Back: the pad's north button is Vedi media here, the quick catalog elsewhere.
///  - Suspended (AnimatingToConsole, Launching, Returning): nothing; A1's game-session panel has the input while a
///    game launches and runs. During the insertion (AnimatingToConsole) any button the frame pressed skips it
///    (anyButtonPressed(), on the frame's actions before any context): it is read by no context.
///  - Edit (I4b step 3, ADR-0015 §4: the editor's): the walking and look actions and Sprint (the walker walks and looks
///    as in FreeRoam), interact (pick up, drop), Back, confirm (the editor's menu: Enter, the pad's Start), edit_pin,
///    the arrows and the D-pad (steps between slots, the menu's focus) and ui_activate (the menu's press). Neither
///    QuickCatalog nor Map: the catalog and the directory stay closed while editing.
/// ToggleDevUI, ReloadShaders and Screenshot are read in every context (isGlobalAction).
enum class WorldContext : std::uint8_t { FreeRoam, Focus, Inspect, Suspended, Edit };

/// "free roam", "focus", "inspect", "suspended", "edit".
[[nodiscard]] std::string_view toString(WorldContext context) noexcept;

[[nodiscard]] constexpr WorldContext worldContextFor(WorldState state) noexcept {
    switch (state) {
    case WorldState::FreeRoam:
        return WorldContext::FreeRoam;
    case WorldState::Focus:
        return WorldContext::Focus;
    case WorldState::Inspect:
        return WorldContext::Inspect;
    case WorldState::AnimatingToConsole:
    case WorldState::Launching:
    case WorldState::Returning:
        return WorldContext::Suspended;
    case WorldState::Edit:
        return WorldContext::Edit;
    }
    return WorldContext::Suspended;
}

/// True when `context` reads `action` (see WorldContext).
[[nodiscard]] bool readsAction(WorldContext context, input::Action action) noexcept;

/// `actions` as `context` reads them: every action it does not read released (actionsFor of input_context.hpp).
[[nodiscard]] input::ActionState actionsFor(WorldContext context, const input::ActionState& actions) noexcept;

/// What the walker (scene::FirstPersonController) is given in `context`: everything FreeRoam reads; in Focus the look
/// actions alone (the feet stay, the eyes move the pick); nothing in Inspect (the look actions turn the item) or while
/// suspended; in Edit what FreeRoam gives, the walking released while an arrow or a D-pad direction is held (the arrow
/// keys walk and step at once otherwise: in Edit they step).
[[nodiscard]] input::ActionState walkerActionsFor(WorldContext context, const input::ActionState& actions) noexcept;

/// True when `actions` pressed a button this frame that skips the insertion (ADR-0016 §11: any button): every action
/// but the look and walking axes, the UI's analog navigation (an axis is no button) and the global actions (a
/// screenshot, the developer overlay, a shader reload, the mute: host functions that change nothing in the world).
[[nodiscard]] bool anyButtonPressed(const input::ActionState& actions) noexcept;

/// Where the pick ray starts (ADR-0012 §2).
enum class PickRaySource : std::uint8_t {
    Reticle, ///< the view's center
    Cursor,  ///< the mouse cursor (the developer overlay open, the mouse free)
};

/// The reticle while the mouse is captured or the pad was used last; the cursor while the mouse is free.
[[nodiscard]] constexpr PickRaySource pickRaySource(bool mouseCaptured, input::DeviceKind lastDevice) noexcept {
    return mouseCaptured || lastDevice == input::DeviceKind::Gamepad ? PickRaySource::Reticle : PickRaySource::Cursor;
}

/// What a left click into the scene means (ADR-0012 §2's click rule).
enum class LeftClick : std::uint8_t {
    CaptureMouse, ///< the mouse is free: the click only captures it and is no interact (platform::MouseCapture
                  ///< consumes it, so it never reaches the action map)
    Interact,     ///< the mouse is captured: the click is interact (the default bindings' left button)
};

[[nodiscard]] constexpr LeftClick leftClickMeaning(bool mouseCaptured) noexcept {
    return mouseCaptured ? LeftClick::Interact : LeftClick::CaptureMouse;
}

/// The world's own Back level (WorldInteraction::backLevel()), which comes after the shell's popups, panel and catalog
/// and before its captured mouse, overlay and quit (back_action.hpp).
enum class WorldBack : std::uint8_t {
    Shell,        ///< the world has nothing to leave: the shell's level
    LeaveInspect, ///< Inspect: back to Focus, the item lowered to its pulled-out pose
    SkipLowering, ///< Focus while the item is still being lowered from Inspect: the lowering ends at once
    LeaveFocus,   ///< Focus: back to FreeRoam, the item put back
};

/// "shell", "leave inspect", "skip lowering", "leave focus".
[[nodiscard]] std::string_view toString(WorldBack level) noexcept;

/// Back in the world: `level` is the world's (WorldBack::Shell to use `shell`), `shell` what backAction() answered.
struct WorldBackDecision {
    WorldBack level = WorldBack::Shell;
    BackAction shell = BackAction::Quit;

    bool operator==(const WorldBackDecision&) const = default;
};

/// Back pressed with the shell in `shell` and the world at `world`: the shell's Ignore, ClosePopup, PanelBack and
/// CatalogBack first; then the world's level unless it is Shell; then the shell's ReleaseMouse, CloseOverlay or Quit.
[[nodiscard]] WorldBackDecision worldBackAction(const BackState& shell, WorldBack world) noexcept;

/// The timings of ADR-0012, in seconds.
struct InteractionTimings {
    /// The hover's dwell on one item before it is pulled out.
    float hoverDwell = 0.150f;
    /// A spine-out item sliding out of its row (a face-out item tilting towards the viewer).
    float pullOut = 0.250f;
    /// Sliding back in.
    float pushBack = 0.200f;
    /// The walker's ease to an item's Focus pose.
    float focusEase = 0.350f;
    /// The walker's ease of "Mostra nella stanza".
    float showInRoomEase = 0.600f;
    /// The lift into the hand (Inspect).
    float lift = 0.300f;
    /// The lowering back to the pulled-out pose (Back from Inspect).
    float lower = 0.200f;
    /// I5 step 9 (ADR-0016 §11): the held item into the console's media socket after Avvia, the eye turning to it.
    float insertion = 0.450f;
};

/// How the inspected item answers the look actions and the zoom (ADR-0012 §4, style §6.2).
struct InspectSettings {
    /// From the eye, meters: where the item is held first, and the zoom's range.
    float distance = 0.45f;
    float minDistance = 0.3f;
    float maxDistance = 1.0f;
    /// Turn rate at full stick deflection (LookX, LookY).
    float stickDegreesPerSecond = 180.0f;
    /// Mouse pixels (LookDeltaX, LookDeltaY) to radians.
    float mouseRadiansPerPixel = 0.005f;
    /// The item's pitch stays within +-pitchLimitDeg; its yaw is unlimited.
    float pitchLimitDeg = 80.0f;
    /// Zoom speed at a fully pressed trigger (ui_page_down brings the item closer, ui_page_up moves it away).
    float zoomMetersPerSecond = 0.5f;
    /// Zoom per wheel notch (away from the user brings the item closer).
    float zoomMetersPerNotch = 0.05f;
};

struct InteractionSettings {
    InteractionTimings timings;
    InspectSettings inspect;
    /// The world's reduceMotion (world::WorldConfig): every animation instant.
    bool reduceMotion = false;
};

/// A collection item as the interaction needs it.
struct RoomItem {
    world::PickId id = world::kNoPick;
    /// What Avvia launches.
    catalog::GameId game;
    /// Face-out items (display cases, the featured shelf, the hero band) tilt instead of sliding out, and interact on
    /// one in FreeRoam inspects it at once.
    bool faceOut = false;
    /// Its shelf section (any number the room chooses: a module, a display case): looking in Focus moves the focus
    /// within it.
    std::uint32_t section = 0;
    /// Part of its fixture (world::CollectionItem::inFixture: an arcade cabinet's screen; the fix of 2026-10-29): the
    /// hover's dwell does not pull it out, so aiming at it moves nothing; Focus and Inspect hold it as any item.
    bool inFixture = false;

    bool operator==(const RoomItem&) const = default;
};

/// The room as the interaction asks it (the app's collection layout from step 10a; a hand-built one in tests).
class InteractionRoom {
public:
    virtual ~InteractionRoom() = default;
    /// The item `id`, or nothing when the room does not hold it (any more).
    [[nodiscard]] virtual std::optional<RoomItem> item(world::PickId id) const = 0;
    /// Where the walker stands to look at `id` in Focus (scene::frontViewpoint of its shelf section at 0.9 m, framed in
    /// the part of the view the card leaves free).
    [[nodiscard]] virtual scene::WalkerPose focusPose(world::PickId id) const = 0;
    /// The D-pad step from `from` (world::SlotNavigation::step).
    [[nodiscard]] virtual std::optional<world::PickId> step(world::PickId from,
                                                            world::StepDirection direction) const = 0;
    /// I5 step 9 (ADR-0016 §11): where the walker looks at the media socket of the room's console from `from` (its
    /// feet kept, the eye turned to the socket), or nothing when the room has no console (the arcade, a shared room, a
    /// hand-made scene): Avvia then launches without the insertion.
    [[nodiscard]] virtual std::optional<scene::WalkerPose> consolePose(const scene::WalkerPose& from) const {
        static_cast<void>(from);
        return std::nullopt;
    }
};

/// A button of the inspected item's card pressed (I3 step 14a, ADR-0012 §5: the card's buttons have the focus, so Enter
/// and the pad's south button press the focused one and a click the one clicked; the app's WorldCard turns them into
/// this entry). Launch is Avvia, Media Vedi media, Info Informazioni, Back Indietro (ui_back's Back levels); Move is
/// "Sposta" (I4b step 3, ADR-0015 §4: the editor entered with the item held).
enum class CardAction : std::uint8_t { Launch, Media, Info, Back, Move };

/// "launch", "media", "info", "back", "move".
[[nodiscard]] std::string_view toString(CardAction action) noexcept;

/// One frame as the interaction sees it.
struct InteractionFrame {
    /// The frame's actions as resolved (before any context), or nullptr while the product UI has the input
    /// (InputContext::Ui): the animations advance, nothing is read and the hover holds.
    const input::ActionState* actions = nullptr;
    /// The frame time, seconds (the fixed one under --fixed-dt); non-finite or negative counts as 0.
    float deltaSeconds = 0.0f;
    /// The walker's pose as the frame starts: where an ease starts.
    scene::WalkerPose walker;
    /// What this frame's pick ray met: world::PickIndex::sample with WorldInteraction::hovered(). Nothing when no ray
    /// was cast (no room, no view).
    std::optional<world::HoverSample> hover;
    /// Mouse wheel notches this frame, + away from the user (zoom in Inspect).
    float wheelNotches = 0.0f;
    /// The shell's levels, read when Back is pressed.
    BackState back;
    /// A button of the card pressed this frame (I3 step 14a), read in Inspect with the frame's actions (ignored without
    /// them): like the action of its button, once the item is in the hand (Back at any time).
    std::optional<CardAction> card;
};

/// Avvia on the inspected item: what to launch (AppCatalog::requestLaunch by GameId, ADR-0012 §6).
struct WorldLaunchRequest {
    world::PickId item = world::kNoPick;
    catalog::GameId game;
    /// I5 step 9 (ADR-0016 §11): the insertion runs: the launch is asked for at once (resolved meanwhile), but its
    /// hand-over waits for InteractionResult::insertionEnded; the screens' decoders begin to stop now.
    bool held = false;

    bool operator==(const WorldLaunchRequest&) const = default;
};

/// How the insertion ended (InteractionResult::insertionEnded).
enum class InsertionEnd : std::uint8_t {
    Arrived, ///< the item reached the console's media socket (InteractionTimings::insertion)
    Skipped, ///< a button skipped the rest of it (anyButtonPressed())
};

/// "arrived", "skipped".
[[nodiscard]] std::string_view toString(InsertionEnd end) noexcept;

/// What the app does after a frame.
struct InteractionResult {
    /// Back was pressed and a level of the shell takes it: the app performs it (the world's own levels are done).
    std::optional<BackAction> shellBack;
    /// QuickCatalog was pressed where the context reads it: open the quick catalog.
    bool openCatalog = false;
    /// Avvia: start this launch; the state stays AnimatingToConsole or Launching until launchEnded().
    std::optional<WorldLaunchRequest> launch;
    /// I5 step 9: the insertion of a held launch ended this frame (AnimatingToConsole left for Launching): its
    /// hand-over may begin.
    std::optional<InsertionEnd> insertionEnded;
    /// Vedi media and Informazioni on the inspected item (I3 step 14b).
    bool showMedia = false;
    bool showInfo = false;
    /// The walker's pose for this frame while an ease or a restore drives it (the app places the walker and the camera
    /// there, and the walker gets no actions); nothing while the player drives it.
    std::optional<scene::WalkerPose> walker;
    /// I4b step 3: "Sposta" on the inspected item's card: the editor is to be entered with this item held (the
    /// application calls enterEdit()); the state stays Inspect until it does.
    std::optional<world::PickId> editMove;
};

/// An item off its static pose: pulled out of its row (spine-out) or tilted (face-out), by `progress`, eased, from 0
/// (in its slot) to 1 (fully out).
struct PulledItem {
    world::PickId id = world::kNoPick;
    float progress = 0.0f;

    bool operator==(const PulledItem&) const = default;
};

/// The inspected item in the hand (ADR-0012 §4).
struct InspectView {
    /// Eased, 0 at the pulled-out pose, 1 held in front of the camera at `distance` (dims the scene behind).
    float lift = 0.0f;
    /// Radians about the item's center: yaw unlimited (wrapped to (-pi, pi]), pitch within the limit; 0, 0 faces the
    /// item's front to the eye.
    float yaw = 0.0f;
    float pitch = 0.0f;
    /// From the eye, meters.
    float distance = 0.45f;

    bool operator==(const InspectView&) const = default;
};

/// Why the last transition failed, for the card (ADR-0012 §1).
enum class InteractionNotice : std::uint8_t {
    None,
    ItemUnavailable, ///< the room no longer holds the item
    LaunchOutcome,   ///< a launch ended in a way A1 tells (refused, failed, crashed): noticeMessage()
};

/// "none", "item unavailable", "launch outcome".
[[nodiscard]] std::string_view toString(InteractionNotice notice) noexcept;

/// What Avvia recorded, to come back to (ADR-0012 §6).
struct WorldLaunchRecord {
    world::PickId item = world::kNoPick;
    catalog::GameId game;
    scene::WalkerPose walker;
    InspectView inspect;
};

class WorldInteraction {
public:
    explicit WorldInteraction(InteractionSettings settings = {});

    /// The room to interact with (it must outlive its use), or nullptr for none (the test room, a room generating):
    /// then roomChanged().
    void setRoom(const InteractionRoom* room);
    /// The room's items changed (a new plan applied): the hover and the pulled items it no longer holds are dropped at
    /// once, and Focus or Inspect of an item it no longer holds falls back to FreeRoam with ItemUnavailable. A launch
    /// in progress is checked at its return.
    void roomChanged();

    /// One frame: the actions of the state's context, then the hover from `frame.hover`, then the animations by the
    /// frame time.
    [[nodiscard]] InteractionResult update(const InteractionFrame& frame);

    /// "Mostra nella stanza" (ADR-0012 §8): Focus of `id` from FreeRoam or Focus, the walker eased from `walker` over
    /// InteractionTimings::showInRoomEase, the item pulled out. False, with ItemUnavailable, when the room does not
    /// hold it; false, changing nothing, in Inspect, AnimatingToConsole, Launching or Returning. The walker's ease is
    /// in the next update's result.
    bool showInRoom(world::PickId id, const scene::WalkerPose& walker);

    /// Focus left at once, its item put back and the walker's ease stopped where it is, with no notice (I4 step 7b:
    /// "Mostra nella stanza" of a game in another room or on another floor leaves Focus first); nothing in other
    /// states.
    void leaveFocusNow();

    /// The launch Avvia asked for ended (A1's LaunchFlow, its outcome as the game-session panel tells it): Returning;
    /// the next update restores the walker's pose and the inspection of the item (in the hand, the insertion undone).
    /// An outcome other than launch_outcome::kReturned (refused, not ready, failed, crashed) becomes the notice. In
    /// AnimatingToConsole too (a launch refused during the insertion: back to Inspect with the item in the hand).
    /// Ignored in any other state.
    void launchEnded(const LaunchMessage& outcome);

    /// Turns every animation instant (and finishes those running), or back.
    void setReduceMotion(bool on);

    /// I4b step 3 (ADR-0015 §4): the state Edit from FreeRoam, Focus or Inspect, the interaction letting go of
    /// everything at once (nothing hovered, focused, pulled or lifted; the walker's ease stopped where it is; no notice).
    /// False, changing nothing, during the insertion, while launching or returning (or in Edit already).
    bool enterEdit();
    /// Edit left for FreeRoam; nothing in other states.
    void leaveEdit();

    [[nodiscard]] WorldState state() const noexcept { return m_state; }
    [[nodiscard]] WorldContext context() const noexcept { return worldContextFor(m_state); }
    /// The world's Back level now (worldBackAction).
    [[nodiscard]] WorldBack backLevel() const noexcept;
    /// The item the pick ray rests on (HoverTracker), in FreeRoam and Focus.
    [[nodiscard]] world::PickId hovered() const noexcept { return m_hover.hovered(); }
    /// The item of Focus, Inspect, AnimatingToConsole, Launching and Returning; kNoPick in FreeRoam.
    [[nodiscard]] world::PickId focused() const noexcept { return m_focused; }
    /// The item outlined and labelled (ADR-0012 §3): the hovered one in FreeRoam, the focused one in Focus, none else.
    [[nodiscard]] world::PickId highlighted() const noexcept;
    /// The items off their static pose, by id: drawn on the dynamic path, their static instances hidden.
    [[nodiscard]] std::span<const PulledItem> pulledItems() const noexcept { return m_pulledView; }
    /// The pull of `id` (0 when it is in its slot).
    [[nodiscard]] float pullOf(world::PickId id) const noexcept;
    [[nodiscard]] InspectView inspectView() const noexcept;
    /// I5 step 9: the insertion's progress, eased, from 0 (the item in the hand) to 1 (in the console's media socket):
    /// it runs in AnimatingToConsole and stays 1 while the game launches and runs; 0 otherwise.
    [[nodiscard]] float insertion() const noexcept { return m_insert.pose(); }
    /// The walker's ease is running.
    [[nodiscard]] bool walkerEasing() const noexcept { return m_walker.animating(); }
    [[nodiscard]] InteractionNotice notice() const noexcept { return m_notice; }
    /// The launch's outcome when notice() is LaunchOutcome.
    [[nodiscard]] const LaunchMessage& noticeMessage() const noexcept { return m_noticeMessage; }
    /// What the last Avvia recorded (Launching, Returning, and after it).
    [[nodiscard]] const std::optional<WorldLaunchRecord>& launchRecord() const noexcept { return m_launch; }
    [[nodiscard]] const InteractionSettings& settings() const noexcept { return m_settings; }

private:
    struct Pull {
        world::PickId id = world::kNoPick;
        scene::PoseAnimator<float> progress;
    };

    void readFreeRoam(const input::ActionState& actions, InteractionResult& result);
    void readFocus(const input::ActionState& actions, InteractionResult& result);
    void readInspect(const input::ActionState& actions, float dt, float wheelNotches, std::optional<CardAction> card,
                     InteractionResult& result);
    /// Avvia on the inspected item: AnimatingToConsole (held) with a console and motion, else Launching.
    void startLaunch(const RoomItem& item, InteractionResult& result);
    /// The insertion's end (arrived or skipped): Launching.
    void endInsertion(InsertionEnd end, InteractionResult& result);
    void back(const BackState& shell, InteractionResult& result);
    void track(const world::HoverSample& sample);
    void finishReturn();

    void enterFocus(world::PickId id, const RoomItem& item, float easeSeconds);
    void moveFocus(world::PickId id, const RoomItem& item, bool easeWalker);
    void enterInspect(world::PickId id, const RoomItem& item);
    void leaveInspect();
    void leaveFocus();
    void toFreeRoam(InteractionNotice notice);
    void easeWalkerTo(const scene::WalkerPose& target, float seconds);
    void skipWalkerAndPull();

    void pull(world::PickId id);
    void pushBack(world::PickId id);
    void pushBackAllBut(world::PickId keep);
    void advancePulls(float dt);
    [[nodiscard]] Pull* findPull(world::PickId id) noexcept;
    [[nodiscard]] std::optional<RoomItem> roomItem(world::PickId id) const;
    void clearNotice();

    InteractionSettings m_settings;
    const InteractionRoom* m_room = nullptr;
    WorldState m_state = WorldState::FreeRoam;
    world::HoverTracker m_hover;
    /// Time the hover has rested on its item, and whether that item may be pulled out (not right after Back put it
    /// back, until the hover leaves it).
    float m_dwell = 0.0f;
    bool m_dwellPulls = true;
    /// The walker's ease and the pull-out under way were started by interact (Focus entered, "Mostra nella stanza"),
    /// so interact skips them; after a D-pad step it inspects at once.
    bool m_interactSkips = false;
    /// Frames since a look action last moved (capped).
    int m_framesSinceLook = 0;
    world::PickId m_focused = world::kNoPick;
    std::uint32_t m_focusedSection = 0;
    std::vector<Pull> m_pulls;
    std::vector<PulledItem> m_pulledView;
    scene::PoseAnimator<scene::WalkerPose> m_walker;
    /// The walker as the frame started (or as showInRoom() was given it).
    scene::WalkerPose m_walkerNow;
    /// The walker was placed this frame without an ease left running (a skip, an instant ease, a stop, a restore).
    bool m_walkerPlaced = false;
    scene::PoseAnimator<float> m_lift;
    /// I5 step 9: the insertion's progress (AnimatingToConsole).
    scene::PoseAnimator<float> m_insert;
    float m_inspectYaw = 0.0f;
    float m_inspectPitch = 0.0f;
    float m_inspectDistance = 0.45f;
    InteractionNotice m_notice = InteractionNotice::None;
    LaunchMessage m_noticeMessage;
    std::optional<WorldLaunchRecord> m_launch;
    std::optional<LaunchMessage> m_launchOutcome;
};

} // namespace rv::app
