#pragma once

// GPU-free (part of rv_app_cli, header only): the asset paths, element ids and data model that the elevator's prompt
// (rv_app's ElevatorPromptPanel, I4 step 7b; ADR-0013 §10: "a proximity zone with a prompt") shares with its RmlUi
// document (assets/ui/elevator_prompt.rml), so that GPU-free tests check the document against them
// (tests/unit/app/test_elevator_prompt_document.cpp); and the name of the key or button the prompt shows. The words are
// the document's: the code passes the key's name.

#include <cctype>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include <rv/input/action.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/binding.hpp>
#include <rv/input/codes.hpp>

namespace rv::app {

namespace elevator_prompt {

inline constexpr std::string_view kDocumentPath = "ui/elevator_prompt.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/elevator_prompt.rcss";
/// The data model: "key" (the name of the interact key or button to press: "E", "A").
inline constexpr std::string_view kDataModel = "elevator_prompt";
inline constexpr std::string_view kPanelId = "elevator-prompt";

} // namespace elevator_prompt

/// The name of a gamepad button as the prompt shows it: the face buttons by their Xbox letters (South "A", East "B",
/// West "X", North "Y"), the others by their binding name.
[[nodiscard]] inline std::string gamepadButtonLabel(input::GamepadButton button) {
    switch (button) {
    case input::GamepadButton::South:
        return "A";
    case input::GamepadButton::East:
        return "B";
    case input::GamepadButton::West:
        return "X";
    case input::GamepadButton::North:
        return "Y";
    default:
        return std::string(input::gamepadButtonName(button));
    }
}

/// The key (`device` KeyboardMouse: the first key bound to Interact, its name in capitals) or button (Gamepad: the first
/// button bound to it) the prompt names for `action` in `map`; "" when nothing of that device is bound to it.
[[nodiscard]] inline std::string promptKeyLabel(const input::ActionMap& map, input::Action action,
                                                input::DeviceKind device) {
    for (const input::Binding& binding : map.bindings(action)) {
        if (device == input::DeviceKind::KeyboardMouse) {
            if (const auto* key = std::get_if<input::KeyBinding>(&binding)) {
                std::string name(input::keyName(key->key));
                for (char& c : name) {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
                return name;
            }
        } else if (const auto* button = std::get_if<input::GamepadButtonBinding>(&binding)) {
            return gamepadButtonLabel(button->button);
        }
    }
    return {};
}

} // namespace rv::app
