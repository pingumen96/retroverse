#include <rv/input/codes.hpp>

#include <array>

namespace rv::input {
namespace {

template <class Code>
struct NamedCode {
    Code code;
    std::string_view name;
};

/// Compile-time table check: entry i describes code i (no gaps, no reordering) and its name is a non-empty
/// lowercase identifier. Name uniqueness is covered by the unit tests (round trip of every code).
template <class Code, std::size_t N>
constexpr bool isDenseLowercaseTable(const std::array<NamedCode<Code>, N>& table) {
    for (std::size_t i = 0; i < N; ++i) {
        if (static_cast<std::size_t>(table[i].code) != i || table[i].name.empty()) {
            return false;
        }
        for (const char c : table[i].name) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                return false;
            }
        }
    }
    return true;
}

template <class Code, std::size_t N>
std::string_view nameOf(const std::array<NamedCode<Code>, N>& table, Code code) noexcept {
    const auto index = static_cast<std::size_t>(code);
    return index < N ? table[index].name : std::string_view("unknown");
}

template <class Code, std::size_t N>
std::optional<Code> codeFromName(const std::array<NamedCode<Code>, N>& table, std::string_view name) noexcept {
    for (const NamedCode<Code>& entry : table) {
        if (entry.name == name) {
            return entry.code;
        }
    }
    return std::nullopt;
}

constexpr std::array<NamedCode<Key>, kKeyCount> kKeyNames{{
    {Key::A, "a"},
    {Key::B, "b"},
    {Key::C, "c"},
    {Key::D, "d"},
    {Key::E, "e"},
    {Key::F, "f"},
    {Key::G, "g"},
    {Key::H, "h"},
    {Key::I, "i"},
    {Key::J, "j"},
    {Key::K, "k"},
    {Key::L, "l"},
    {Key::M, "m"},
    {Key::N, "n"},
    {Key::O, "o"},
    {Key::P, "p"},
    {Key::Q, "q"},
    {Key::R, "r"},
    {Key::S, "s"},
    {Key::T, "t"},
    {Key::U, "u"},
    {Key::V, "v"},
    {Key::W, "w"},
    {Key::X, "x"},
    {Key::Y, "y"},
    {Key::Z, "z"},
    {Key::Digit0, "0"},
    {Key::Digit1, "1"},
    {Key::Digit2, "2"},
    {Key::Digit3, "3"},
    {Key::Digit4, "4"},
    {Key::Digit5, "5"},
    {Key::Digit6, "6"},
    {Key::Digit7, "7"},
    {Key::Digit8, "8"},
    {Key::Digit9, "9"},
    {Key::F1, "f1"},
    {Key::F2, "f2"},
    {Key::F3, "f3"},
    {Key::F4, "f4"},
    {Key::F5, "f5"},
    {Key::F6, "f6"},
    {Key::F7, "f7"},
    {Key::F8, "f8"},
    {Key::F9, "f9"},
    {Key::F10, "f10"},
    {Key::F11, "f11"},
    {Key::F12, "f12"},
    {Key::Escape, "escape"},
    {Key::Enter, "enter"},
    {Key::Tab, "tab"},
    {Key::Space, "space"},
    {Key::Backspace, "backspace"},
    {Key::Up, "up"},
    {Key::Down, "down"},
    {Key::Left, "left"},
    {Key::Right, "right"},
    {Key::LeftShift, "left_shift"},
    {Key::RightShift, "right_shift"},
    {Key::LeftCtrl, "left_ctrl"},
    {Key::RightCtrl, "right_ctrl"},
    {Key::LeftAlt, "left_alt"},
    {Key::RightAlt, "right_alt"},
    {Key::PageUp, "page_up"},
    {Key::PageDown, "page_down"},
    {Key::Home, "home"},
    {Key::End, "end"},
    {Key::Insert, "insert"},
    {Key::Delete, "delete"},
    {Key::Grave, "grave"},
    {Key::Minus, "minus"},
    {Key::Equals, "equals"},
}};
static_assert(isDenseLowercaseTable(kKeyNames), "kKeyNames must list every Key in enum order");

constexpr std::array<NamedCode<MouseButton>, kMouseButtonCount> kMouseButtonNames{{
    {MouseButton::Left, "left"},
    {MouseButton::Right, "right"},
    {MouseButton::Middle, "middle"},
    {MouseButton::X1, "x1"},
    {MouseButton::X2, "x2"},
}};
static_assert(isDenseLowercaseTable(kMouseButtonNames), "kMouseButtonNames must list every MouseButton in order");

constexpr std::array<NamedCode<MouseAxis>, kMouseAxisCount> kMouseAxisNames{{
    {MouseAxis::DeltaX, "delta_x"},
    {MouseAxis::DeltaY, "delta_y"},
    {MouseAxis::Wheel, "wheel"},
}};
static_assert(isDenseLowercaseTable(kMouseAxisNames), "kMouseAxisNames must list every MouseAxis in order");

constexpr std::array<NamedCode<GamepadButton>, kGamepadButtonCount> kGamepadButtonNames{{
    {GamepadButton::South, "south"},
    {GamepadButton::East, "east"},
    {GamepadButton::West, "west"},
    {GamepadButton::North, "north"},
    {GamepadButton::Back, "back"},
    {GamepadButton::Guide, "guide"},
    {GamepadButton::Start, "start"},
    {GamepadButton::LeftStick, "left_stick"},
    {GamepadButton::RightStick, "right_stick"},
    {GamepadButton::LeftShoulder, "left_shoulder"},
    {GamepadButton::RightShoulder, "right_shoulder"},
    {GamepadButton::DpadUp, "dpad_up"},
    {GamepadButton::DpadDown, "dpad_down"},
    {GamepadButton::DpadLeft, "dpad_left"},
    {GamepadButton::DpadRight, "dpad_right"},
}};
static_assert(isDenseLowercaseTable(kGamepadButtonNames), "kGamepadButtonNames must list every GamepadButton in order");

constexpr std::array<NamedCode<GamepadAxis>, kGamepadAxisCount> kGamepadAxisNames{{
    {GamepadAxis::LeftX, "left_x"},
    {GamepadAxis::LeftY, "left_y"},
    {GamepadAxis::RightX, "right_x"},
    {GamepadAxis::RightY, "right_y"},
    {GamepadAxis::LeftTrigger, "left_trigger"},
    {GamepadAxis::RightTrigger, "right_trigger"},
}};
static_assert(isDenseLowercaseTable(kGamepadAxisNames), "kGamepadAxisNames must list every GamepadAxis in order");

} // namespace

std::string_view keyName(Key key) noexcept {
    return nameOf(kKeyNames, key);
}

std::optional<Key> keyFromName(std::string_view name) noexcept {
    return codeFromName(kKeyNames, name);
}

std::string_view mouseButtonName(MouseButton button) noexcept {
    return nameOf(kMouseButtonNames, button);
}

std::optional<MouseButton> mouseButtonFromName(std::string_view name) noexcept {
    return codeFromName(kMouseButtonNames, name);
}

std::string_view mouseAxisName(MouseAxis axis) noexcept {
    return nameOf(kMouseAxisNames, axis);
}

std::optional<MouseAxis> mouseAxisFromName(std::string_view name) noexcept {
    return codeFromName(kMouseAxisNames, name);
}

std::string_view gamepadButtonName(GamepadButton button) noexcept {
    return nameOf(kGamepadButtonNames, button);
}

std::optional<GamepadButton> gamepadButtonFromName(std::string_view name) noexcept {
    return codeFromName(kGamepadButtonNames, name);
}

std::string_view gamepadAxisName(GamepadAxis axis) noexcept {
    return nameOf(kGamepadAxisNames, axis);
}

std::optional<GamepadAxis> gamepadAxisFromName(std::string_view name) noexcept {
    return codeFromName(kGamepadAxisNames, name);
}

} // namespace rv::input
