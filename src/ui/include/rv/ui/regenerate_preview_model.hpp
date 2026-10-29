#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The impact preview of a regeneration and of an undo (I4b step 6; ADR-0015 §5, §6): the panel the directory's
// "Rigenera stanza / piano / mondo" and "Annulla ultima modifica" open (assets/ui/regenerate_preview.rml), and the
// update's needsConfirmation notice. While the app's dry run runs on the pool the panel says so (Computing, Annulla
// only); then it shows what the regeneration would change (the rooms regenerated, added, removed and moved, the games
// that change slot, the titles entering and leaving face-out, the games newly placed and unplaced, the fixtures added,
// removed and moved, the customizations kept and those that cannot be honoured, each with its reason) and the three
// choices of ADR-0015 §5: "Rigenera" (the customizations kept; only when something would change), "Rigenera e scarta
// le personalizzazioni" (only when the scope has some) and "Annulla". The second choice asks a second confirmation that
// names how many customizations go (Confirm: "Indietro", focused, above "Scarta e rigenera", so that a button pressed
// twice, or clicked twice, never drops anything: the choices stand at the panel's bottom, the drop's row is
// Indietro's). An undo shows the action it would undo (its kind, its scope, when) with "Annulla la modifica" and
// "Indietro". Once a choice is made the panel waits for the commit (Committing: no button), and a
// failure is told with "Chiudi". The focus a content arrives with never rests on a choice that cannot be taken back
// (I4b step 6's review): "Rigenera" (an undo takes it back), else "Annulla" (an undo's "Indietro", a preview that
// only offers the drop).
//
// Input (the ui_* actions through NavInput): Up and Left move to the previous button, Down and Right to the next;
// Activate (or a click) presses the focused button, but for the first input frame after the content arrived (a held or
// doubled press meant for what was there before never lands on a choice); Back leaves the second confirmation for the
// preview (Drop focused), else closes the panel (Cancel), never while committing. The hints show the glyphs of the
// device used last. Every sentence is the document's: the model holds keys and counts only.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

/// The ids, the data model and the keys the preview's document (assets/ui/regenerate_preview.rml) and its screen
/// (rv::ui::RegeneratePreviewScreen) share (tests/unit/ui/test_regenerate_preview.cpp checks the document against
/// them).
namespace regenerate_preview {
inline constexpr std::string_view kDocumentPath = "ui/regenerate_preview.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/regenerate_preview.rcss";
inline constexpr std::string_view kDataModel = "regenerate_preview";
inline constexpr std::string_view kPanelId = "regenerate-preview";
/// The buttons (RegenerateButton), in the order they stand.
inline constexpr std::string_view kRegenerateId = "rp-regenerate";
inline constexpr std::string_view kDropId = "rp-drop";
inline constexpr std::string_view kConfirmDropId = "rp-confirm-drop";
inline constexpr std::string_view kUndoId = "rp-undo";
inline constexpr std::string_view kBackId = "rp-back";
inline constexpr std::string_view kCancelId = "rp-cancel";
/// The data event of a click on a button: "button('<regenerate|drop|confirm-drop|undo|back|cancel>')".
inline constexpr std::string_view kButtonEvent = "button";
/// The device whose glyphs the hints show ("device"): the pad's buttons, or the keys.
inline constexpr std::array<std::string_view, 2> kDevices{"pad", "keys"};
/// What the panel is about (RegenerateKind).
inline constexpr std::array<std::string_view, 4> kKinds{"room", "floor", "world", "undo"};
/// Where it stands (RegenerateState).
inline constexpr std::array<std::string_view, 5> kStates{"computing", "ready", "confirm", "committing", "failed"};
/// A conflict's reason (world::ConflictReason's names) and its subject (a game, or a fixture by its role).
inline constexpr std::array<std::string_view, 4> kConflictReasons{"room-gone", "slot-gone", "game-gone", "invalid"};
inline constexpr std::array<std::string_view, 4> kConflictSubjects{"game", "module", "display", "decoration"};
/// Why the preview or the commit failed (each has a sentence): an action in progress or the world changed meanwhile
/// (busy), the media unknown (nothing is decided from them), the plan refused or broken by the regeneration, nothing to
/// undo, a damaged snapshot, anything else.
inline constexpr std::array<std::string_view, 8> kFailures{
    "busy", "changed", "media-unknown", "invalid-plan", "nothing-to-undo", "damaged", "no-plan", "failed"};
/// The explicit actions an undo names (world snapshots' actions with a sentence; another shows generically).
inline constexpr std::array<std::string_view, 2> kUndoActions{"edit", "regenerate"};
/// The conflicts listed by name at most (the others counted).
inline constexpr std::size_t kConflictsListed = 5;
} // namespace regenerate_preview

/// What the panel is about.
enum class RegenerateKind : std::uint8_t { Room, Floor, World, Undo };
/// "room", "floor", "world", "undo".
[[nodiscard]] std::string_view toString(RegenerateKind kind) noexcept;

enum class RegenerateState : std::uint8_t {
    Computing,  ///< the dry run runs on the pool
    Ready,      ///< the impact (or the undo's action) shows with its choices
    Confirm,    ///< the second confirmation of "Rigenera e scarta le personalizzazioni"
    Committing, ///< the choice is being written and shown
    Failed,     ///< the preview or the commit failed (its key)
};
/// "computing", "ready", "confirm", "committing", "failed".
[[nodiscard]] std::string_view toString(RegenerateState state) noexcept;

enum class RegenerateButton : std::uint8_t { Regenerate, Drop, ConfirmDrop, Undo, Back, Cancel };
/// "regenerate", "drop", "confirm-drop", "undo", "back", "cancel" (the button event's argument).
[[nodiscard]] std::string_view toString(RegenerateButton button) noexcept;
[[nodiscard]] std::optional<RegenerateButton> regenerateButtonFromName(std::string_view name) noexcept;
/// The button's element id (regenerate_preview::kRegenerateId ...).
[[nodiscard]] std::string_view elementIdOf(RegenerateButton button) noexcept;

/// What the scope is, as the panel names it: a room by its kind ("console", "shared", "arcade") and its system's name
/// (a console room's), a floor by its building.json id (the directory's names), the world.
struct RegenerateScopeTitle {
    std::string roomKind;
    std::string roomTitle;
    std::string floor;

    bool operator==(const RegenerateScopeTitle&) const = default;
};

/// A customization the regeneration cannot honour: its reason (kConflictReasons), its subject (kConflictSubjects) and
/// the game's title (a fixture's: empty).
struct RegenerateConflict {
    std::string reason;
    std::string subject;
    std::string title;

    bool operator==(const RegenerateConflict&) const = default;
};

/// The preview's content: a regeneration's impact (world::RegenerationImpact's counts) or the undo's action.
struct RegeneratePreviewContent {
    std::uint32_t roomsRegenerated = 0;
    std::uint32_t roomsAdded = 0;
    std::uint32_t roomsRemoved = 0;
    std::uint32_t roomsMoved = 0;
    std::uint32_t gamesMoved = 0;
    std::uint32_t faceOutEntering = 0;
    std::uint32_t faceOutLeaving = 0;
    std::uint32_t newlyPlaced = 0;
    std::uint32_t newlyUnplaced = 0;
    std::uint32_t fixturesAdded = 0;
    std::uint32_t fixturesRemoved = 0;
    std::uint32_t fixturesMoved = 0;
    /// The scope's customizations the new plan honours, and those it cannot: all of them (`conflictCount`), the first
    /// regenerate_preview::kConflictsListed listed.
    std::uint32_t pinsKept = 0;
    std::uint32_t conflictCount = 0;
    std::vector<RegenerateConflict> conflicts;
    /// The scope's customizations "Rigenera e scarta le personalizzazioni" drops (0: the choice does not show).
    std::uint32_t overrides = 0;
    /// "Rigenera" would change the plan or the overrides (else it does not show: nothing to regenerate).
    bool changed = false;
    /// An undo: the action it undoes (a world snapshot's action: kUndoActions has its sentence), its scope as the
    /// snapshot records it ("room:snes", "floor:gen4", "world"; the panel names a room or a floor by
    /// `undoScopeTitle`), when it was taken ("dd/mm/yyyy hh:mm", local time; empty: unknown).
    std::string undoAction;
    std::string undoScope;
    RegenerateScopeTitle undoScopeTitle;
    std::string undoWhen;

    bool operator==(const RegeneratePreviewContent&) const = default;
};

/// What the panel asks of the app.
enum class RegenerateCommandKind : std::uint8_t {
    Regenerate,     ///< "Rigenera": the overrides kept
    RegenerateDrop, ///< "Scarta e rigenera" (after the second confirmation): the scope's overrides dropped
    Undo,           ///< "Annulla la modifica"
    Cancel,         ///< the panel closed without a choice (Annulla, Indietro, Back)
};
/// "regenerate", "regenerate-drop", "undo", "cancel".
[[nodiscard]] std::string_view toString(RegenerateCommandKind kind) noexcept;

/// The preview's state: what it is about, where it stands, its content, its focused button. Main thread; no rendering.
/// Every change of what the panel shows increments revision().
class RegeneratePreviewModel {
public:
    /// Opens the panel on `kind` for `scope`, computing (Annulla focused).
    void open(RegenerateKind kind, RegenerateScopeTitle scope);
    /// The dry run's answer: Ready, its default button focused ("Rigenera" when it would change something, else
    /// Annulla; an undo's "Indietro"), Activate ignored until the next endInputFrame(). Ignored unless computing.
    void setContent(RegeneratePreviewContent content);
    /// An input frame ended (the product UI's update applied its commands; the content arrives between two of them, in
    /// the world's update): Activate counts again.
    void endInputFrame() noexcept;
    /// The device used last: the hints show the pad's glyphs (`pad`), else the keys'.
    void setDevice(bool pad) noexcept;
    [[nodiscard]] bool padDevice() const noexcept { return m_pad; }
    /// The preview or the commit failed (regenerate_preview::kFailures): Failed, Annulla focused (its sentence says
    /// why). Ignored while closed.
    void setFailed(std::string failure);
    void close() noexcept;

    /// A UI command (see the header). The command for the app, if any.
    [[nodiscard]] std::optional<RegenerateCommandKind> apply(UiCommand command);
    /// A click on `button` (shown): it is focused and pressed.
    [[nodiscard]] std::optional<RegenerateCommandKind> click(RegenerateButton button);

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    [[nodiscard]] RegenerateKind kind() const noexcept { return m_kind; }
    [[nodiscard]] RegenerateState state() const noexcept { return m_state; }
    [[nodiscard]] const RegenerateScopeTitle& scope() const noexcept { return m_scope; }
    [[nodiscard]] const RegeneratePreviewContent& content() const noexcept { return m_content; }
    [[nodiscard]] const std::string& failure() const noexcept { return m_failure; }
    /// The buttons shown, in focus order (none while committing).
    [[nodiscard]] std::vector<RegenerateButton> buttons() const;
    /// The focused button (meaningless while none shows).
    [[nodiscard]] RegenerateButton focused() const noexcept { return m_focused; }
    /// The focused button's element id ("" while none shows).
    [[nodiscard]] std::string focusedElement() const;
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void changed() noexcept { ++m_revision; }
    void focus(RegenerateButton button) noexcept;
    std::optional<RegenerateCommandKind> press(RegenerateButton button);
    std::optional<RegenerateCommandKind> cancel();

    bool m_open = false;
    RegenerateKind m_kind = RegenerateKind::Room;
    RegenerateState m_state = RegenerateState::Computing;
    RegenerateScopeTitle m_scope;
    RegeneratePreviewContent m_content;
    std::string m_failure;
    RegenerateButton m_focused = RegenerateButton::Cancel;
    std::uint64_t m_revision = 0;
    /// Activate is ignored until the input frame after a content arrived ends; the device used last.
    bool m_activateHeld = false;
    bool m_pad = false;
};

} // namespace rv::ui
