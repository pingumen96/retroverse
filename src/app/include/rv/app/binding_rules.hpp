#pragma once

// GPU-free (part of rv_app_cli): no SDL or OpenGL header is included.
//
// Remapping's rules (ADR-0018 §7): which actions the Controlli group lists, what a press captured from the raw input means
// for one of them (free, a clash judged per context, reserved, a lock-out refused), and the edits it makes. An action has
// one slot per device: a press replaces the action's first key, or its first pad button or trigger, and nothing else, so
// Interact keeps its click.

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <rv/input/action.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/binding.hpp>
#include <rv/input/raw_input.hpp>

namespace rv::app {

/// The two lists of the Controlli group.
enum class BindingGroup : std::uint8_t { World, Ui };

struct BindableAction {
    input::Action action = input::Action::Interact;
    BindingGroup group = BindingGroup::World;
};

/// The actions a user may bind, in the screen's order: buttons only (the stick and the camera stay; the arrows and the
/// D-pad stay, so that the screen can always be walked), no developer's key.
[[nodiscard]] std::span<const BindableAction> bindableActions() noexcept;
[[nodiscard]] bool isBindable(input::Action action) noexcept;

/// UiActivate, UiBack, Menu: they always keep a pad button and a key.
[[nodiscard]] bool isLockedAction(input::Action action) noexcept;

/// A key a text field needs (a letter, a digit, the space bar, the characters, Backspace, Canc): no UI action takes one.
[[nodiscard]] bool isTextKey(input::Key key) noexcept;

/// True when one context reads both (input_context.hpp: world and UI share buttons on purpose), the shipped sharings
/// aside: QuickCatalog with UiMedia (catalogToggleRequested()), Confirm with Menu.
[[nodiscard]] bool actionsCompete(input::Action left, input::Action right) noexcept;

enum class BindingVerdict : std::uint8_t {
    Free,     ///< nothing else in its contexts uses it
    Same,     ///< the action has it already
    Clash,    ///< other actions a user may bind have it: a swap is offered
    Reserved, ///< it stays what it is: an action not listed uses it, a text field needs it, or what a swap would
              ///< hand back is another action's
    Locked,   ///< the action has nothing to give back, and a holder would be left with no key or no pad button
};
[[nodiscard]] std::string_view toString(BindingVerdict verdict) noexcept;

struct BindingJudgement {
    BindingVerdict verdict = BindingVerdict::Free;
    /// Who has it, or who refuses what a swap hands back (Clash, Locked, and Reserved by an action): the first of them.
    std::optional<input::Action> other;

    bool operator==(const BindingJudgement&) const = default;
};

/// What `press` means for `action` on `map`.
[[nodiscard]] BindingJudgement judgeBinding(const input::ActionMap& map, input::Action action,
                                            const input::Binding& press);

/// `map` with `press` in `action`'s slot of that device.
[[nodiscard]] input::ActionMap withBinding(input::ActionMap map, input::Action action, const input::Binding& press);
/// Every action that holds `press` in a context `action` is read in (actionsCompete()): whom a swap involves.
[[nodiscard]] std::vector<input::Action> bindingHolders(const input::ActionMap& map, input::Action action,
                                                        const input::Binding& press);
/// `map` with `press` in `action`'s slot, and what that slot held given to every bindingHolders() in place of `press`
/// (they lose it when the slot was empty, which judgeBinding() allows only to a holder with another of that device).
[[nodiscard]] input::ActionMap withSwap(input::ActionMap map, input::Action action, const input::Binding& press);

/// `action`'s bindings as a binding file's list (settings.json's section).
[[nodiscard]] nlohmann::json bindingList(const input::ActionMap& map, input::Action action);

/// No lock-out (ADR-0018 §7), whatever a file says: each locked action of `map` without a key or a pad button gets
/// `shipped`'s back. The actions it gave one to.
std::vector<input::Action> keepUnlocked(input::ActionMap& map, const input::ActionMap& shipped);

/// A press read from the raw input while a capture runs: a key's or a pad button's edge, a trigger crossing its
/// half-way mark. The mouse is never captured: it is what clicks the screen.
class CapturePress {
public:
    /// Starts over; `frame` holds what is down now, which never counts.
    void start(const input::RawInputFrame& frame) noexcept;
    /// The press of `frame`, if any.
    [[nodiscard]] std::optional<input::Binding> next(const input::RawInputFrame& frame) noexcept;

private:
    std::array<bool, 2> m_triggers{};
};

} // namespace rv::app
