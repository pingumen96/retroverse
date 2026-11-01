#include <rv/app/binding_rules.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include <rv/app/input_context.hpp>
#include <rv/input/binding_labels.hpp>

namespace rv::app {
namespace {

using input::Action;
using input::Binding;
using input::DeviceKind;
using input::GamepadAxis;
using input::GamepadAxisBinding;
using input::GamepadButtonBinding;
using input::Key;
using input::KeyBinding;

constexpr std::array<BindableAction, 21> kBindable = {{
    {Action::Interact, BindingGroup::World},
    {Action::Back, BindingGroup::World},
    {Action::QuickCatalog, BindingGroup::World},
    {Action::Map, BindingGroup::World},
    {Action::Menu, BindingGroup::World},
    {Action::Confirm, BindingGroup::World},
    {Action::Sprint, BindingGroup::World},
    {Action::ToggleMute, BindingGroup::World},
    {Action::EditPin, BindingGroup::World},
    {Action::EditRotateLeft, BindingGroup::World},
    {Action::EditRotateRight, BindingGroup::World},
    {Action::UiActivate, BindingGroup::Ui},
    {Action::UiBack, BindingGroup::Ui},
    {Action::UiMedia, BindingGroup::Ui},
    {Action::UiInfo, BindingGroup::Ui},
    {Action::UiFilterPrevious, BindingGroup::Ui},
    {Action::UiFilterNext, BindingGroup::Ui},
    {Action::UiPageUp, BindingGroup::Ui},
    {Action::UiPageDown, BindingGroup::Ui},
    {Action::UiHome, BindingGroup::Ui},
    {Action::UiEnd, BindingGroup::Ui},
}};

constexpr std::array<Action, 3> kLocked = {Action::UiActivate, Action::UiBack, Action::Menu};

[[nodiscard]] DeviceKind deviceOf(const Binding& binding) noexcept {
    return std::holds_alternative<GamepadButtonBinding>(binding) || std::holds_alternative<GamepadAxisBinding>(binding)
               ? DeviceKind::Gamepad
               : DeviceKind::KeyboardMouse;
}

/// A slot's device: a key's slot holds keys alone (a click stays), a pad's its buttons and triggers.
[[nodiscard]] bool inSlot(const Binding& binding, DeviceKind device) noexcept {
    return device == DeviceKind::Gamepad ? deviceOf(binding) == DeviceKind::Gamepad
                                         : std::holds_alternative<KeyBinding>(binding);
}

[[nodiscard]] std::optional<Binding> slotOf(const input::ActionMap& map, Action action, DeviceKind device) {
    for (const Binding& binding : map.bindings(action)) {
        if (inSlot(binding, device)) {
            return binding;
        }
    }
    return std::nullopt;
}

/// `map` with `action`'s bindings rewritten: `from` (none: nothing) becomes `to` (none: gone); `to` goes first in the
/// action's slot when `from` is none.
void replaceBinding(input::ActionMap& map, Action action, const std::optional<Binding>& from,
                    const std::optional<Binding>& to) {
    const std::vector<Binding> old(map.bindings(action).begin(), map.bindings(action).end());
    map.clear(action);
    bool placed = false;
    for (const Binding& binding : old) {
        if (from && input::sameInput(binding, *from) && !placed) {
            if (to) {
                map.bind(action, *to);
            }
            placed = true;
        } else {
            map.bind(action, binding);
        }
    }
    if (!placed && to) {
        map.bind(action, *to);
    }
}

} // namespace

std::span<const BindableAction> bindableActions() noexcept {
    return kBindable;
}

bool isBindable(Action action) noexcept {
    return std::any_of(kBindable.begin(), kBindable.end(),
                       [&](const BindableAction& entry) { return entry.action == action; });
}

bool isLockedAction(Action action) noexcept {
    return std::find(kLocked.begin(), kLocked.end(), action) != kLocked.end();
}

bool isTextKey(Key key) noexcept {
    const std::size_t index = input::toIndex(key);
    return index <= input::toIndex(Key::Digit9) || key == Key::Space || key == Key::Backspace || key == Key::Delete ||
           key == Key::Grave || key == Key::Minus || key == Key::Equals;
}

bool actionsCompete(Action left, Action right) noexcept {
    if (left == right) {
        return false;
    }
    const auto pair = [&](Action a, Action b) { return (left == a && right == b) || (left == b && right == a); };
    if (pair(Action::QuickCatalog, Action::UiMedia) || pair(Action::Confirm, Action::Menu)) {
        return false;
    }
    return (readsAction(InputContext::World, left) && readsAction(InputContext::World, right)) ||
           (readsAction(InputContext::Ui, left) && readsAction(InputContext::Ui, right));
}

std::string_view toString(BindingVerdict verdict) noexcept {
    switch (verdict) {
    case BindingVerdict::Free:
        return "free";
    case BindingVerdict::Same:
        return "same";
    case BindingVerdict::Clash:
        return "clash";
    case BindingVerdict::Reserved:
        return "reserved";
    case BindingVerdict::Locked:
        return "locked";
    }
    return "unknown";
}

BindingJudgement judgeBinding(const input::ActionMap& map, Action action, const Binding& press) {
    const DeviceKind device = deviceOf(press);
    const std::optional<Binding> slot = slotOf(map, action, device);
    if (slot && input::sameInput(*slot, press)) {
        return {BindingVerdict::Same, std::nullopt};
    }
    if (const auto* key = std::get_if<KeyBinding>(&press); key != nullptr && input::isUiAction(action) &&
                                                           isTextKey(key->key)) {
        return {BindingVerdict::Reserved, std::nullopt};
    }
    std::optional<Action> clash;
    for (std::size_t index = 0; index < input::kActionCount; ++index) {
        const auto other = static_cast<Action>(index);
        if (!actionsCompete(action, other)) {
            continue;
        }
        const auto bindings = map.bindings(other);
        const bool holds = std::any_of(bindings.begin(), bindings.end(),
                                       [&](const Binding& binding) { return input::sameInput(binding, press); });
        if (!holds) {
            continue;
        }
        if (!isBindable(other)) {
            return {BindingVerdict::Reserved, other};
        }
        clash = clash ? clash : std::optional<Action>(other);
    }
    if (!clash) {
        return {BindingVerdict::Free, std::nullopt};
    }
    if (isLockedAction(*clash) && !slot) {
        return {BindingVerdict::Locked, clash};
    }
    return {BindingVerdict::Clash, clash};
}

input::ActionMap withBinding(input::ActionMap map, Action action, const Binding& press) {
    replaceBinding(map, action, slotOf(map, action, deviceOf(press)), press);
    return map;
}

input::ActionMap withSwap(input::ActionMap map, Action action, const Binding& press, Action other) {
    const std::optional<Binding> slot = slotOf(map, action, deviceOf(press));
    replaceBinding(map, action, slot, press);
    replaceBinding(map, other, press, slot);
    return map;
}

nlohmann::json bindingList(const input::ActionMap& map, Action action) {
    return map.toJson()["bindings"][std::string(input::actionName(action))];
}

std::vector<Action> keepUnlocked(input::ActionMap& map, const input::ActionMap& shipped) {
    std::vector<Action> kept;
    for (const Action action : kLocked) {
        for (const DeviceKind device : {DeviceKind::KeyboardMouse, DeviceKind::Gamepad}) {
            if (slotOf(map, action, device)) {
                continue;
            }
            if (const std::optional<Binding> first = slotOf(shipped, action, device)) {
                map.bind(action, *first);
                kept.push_back(action);
            }
        }
    }
    return kept;
}

void CapturePress::start(const input::RawInputFrame& frame) noexcept {
    m_triggers[0] = frame.gamepadAxes[input::toIndex(GamepadAxis::LeftTrigger)] >= 0.5f;
    m_triggers[1] = frame.gamepadAxes[input::toIndex(GamepadAxis::RightTrigger)] >= 0.5f;
}

std::optional<Binding> CapturePress::next(const input::RawInputFrame& frame) noexcept {
    for (std::size_t index = 0; index < input::kKeyCount; ++index) {
        if (frame.keysPressed[index]) {
            return Binding{KeyBinding{static_cast<Key>(index), 1.0f}};
        }
    }
    if (!frame.gamepadConnected) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < input::kGamepadButtonCount; ++index) {
        if (frame.gamepadButtonsPressed[index]) {
            return Binding{GamepadButtonBinding{static_cast<input::GamepadButton>(index), 1.0f}};
        }
    }
    constexpr std::array<GamepadAxis, 2> kTriggers = {GamepadAxis::LeftTrigger, GamepadAxis::RightTrigger};
    for (std::size_t index = 0; index < kTriggers.size(); ++index) {
        const bool down = frame.gamepadAxes[input::toIndex(kTriggers[index])] >= 0.5f;
        const bool pressed = down && !m_triggers[index];
        m_triggers[index] = down;
        if (pressed) {
            return Binding{GamepadAxisBinding{kTriggers[index], 1.0f}};
        }
    }
    return std::nullopt;
}

} // namespace rv::app
