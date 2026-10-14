#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include <rv/input/codes.hpp>

using rv::input::GamepadAxis;
using rv::input::GamepadButton;
using rv::input::Key;
using rv::input::MouseAxis;
using rv::input::MouseButton;

namespace {

/// Every code of a family has a unique name that parses back to the same code, and the set of names is exactly
/// `expected`. The Count sentinel formats as "unknown", which no parser accepts.
template <class Code, class NameFn, class ParseFn>
void checkFamily(std::size_t count, NameFn nameOf, ParseFn parse, const std::set<std::string>& expected) {
    std::set<std::string> names;
    for (std::size_t i = 0; i < count; ++i) {
        const auto code = static_cast<Code>(i);
        const std::string name(nameOf(code));
        INFO("code index " << i << " named \"" << name << "\"");
        CHECK(names.insert(name).second);
        const std::optional<Code> parsed = parse(name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == code);
    }
    CHECK(names == expected);
    CHECK(nameOf(static_cast<Code>(count)) == "unknown");
    CHECK_FALSE(parse("unknown").has_value());
    CHECK_FALSE(parse("").has_value());
}

} // namespace

TEST_CASE("input/codes: key names are unique, lowercase and round-trip", "[input][codes]") {
    CHECK(rv::input::kKeyCount == 72);
    checkFamily<Key>(rv::input::kKeyCount, rv::input::keyName, rv::input::keyFromName,
                     {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
                      "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
                      "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
                      "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
                      "escape", "enter", "tab", "space", "backspace", "up", "down", "left", "right",
                      "left_shift", "right_shift", "left_ctrl", "right_ctrl", "left_alt", "right_alt",
                      "page_up", "page_down", "home", "end", "insert", "delete", "grave", "minus", "equals"});

    // Contiguous ranges map to the expected characters.
    for (int i = 0; i < 26; ++i) {
        const auto key = static_cast<Key>(static_cast<int>(Key::A) + i);
        CHECK(rv::input::keyName(key) == std::string(1, static_cast<char>('a' + i)));
    }
    for (int i = 0; i < 10; ++i) {
        const auto key = static_cast<Key>(static_cast<int>(Key::Digit0) + i);
        CHECK(rv::input::keyName(key) == std::string(1, static_cast<char>('0' + i)));
    }
    for (int i = 0; i < 12; ++i) {
        const auto key = static_cast<Key>(static_cast<int>(Key::F1) + i);
        CHECK(rv::input::keyName(key) == "f" + std::to_string(i + 1));
    }
    CHECK(rv::input::keyName(Key::Grave) == "grave");
    CHECK(rv::input::keyName(Key::LeftCtrl) == "left_ctrl");
    CHECK(rv::input::keyName(Key::PageDown) == "page_down");
    CHECK(rv::input::keyName(Key::Delete) == "delete");
    CHECK(rv::input::keyName(Key::Enter) == "enter");

    // Parsing is exact: binding files use the lowercase names only.
    CHECK_FALSE(rv::input::keyFromName("W").has_value());
    CHECK_FALSE(rv::input::keyFromName("F1").has_value());
    CHECK_FALSE(rv::input::keyFromName(" w").has_value());
    CHECK_FALSE(rv::input::keyFromName("f13").has_value());
    CHECK_FALSE(rv::input::keyFromName("lshift").has_value());
}

TEST_CASE("input/codes: mouse button and axis names round-trip", "[input][codes]") {
    CHECK(rv::input::kMouseButtonCount == 5);
    checkFamily<MouseButton>(rv::input::kMouseButtonCount, rv::input::mouseButtonName,
                             rv::input::mouseButtonFromName, {"left", "right", "middle", "x1", "x2"});
    CHECK(rv::input::mouseButtonName(MouseButton::Middle) == "middle");
    CHECK(rv::input::mouseButtonName(MouseButton::X2) == "x2");

    CHECK(rv::input::kMouseAxisCount == 3);
    checkFamily<MouseAxis>(rv::input::kMouseAxisCount, rv::input::mouseAxisName, rv::input::mouseAxisFromName,
                           {"delta_x", "delta_y", "wheel"});
    CHECK(rv::input::mouseAxisName(MouseAxis::DeltaY) == "delta_y");
    CHECK(rv::input::mouseAxisName(MouseAxis::Wheel) == "wheel");
}

TEST_CASE("input/codes: gamepad button and axis names round-trip", "[input][codes]") {
    CHECK(rv::input::kGamepadButtonCount == 15);
    checkFamily<GamepadButton>(rv::input::kGamepadButtonCount, rv::input::gamepadButtonName,
                               rv::input::gamepadButtonFromName,
                               {"south", "east", "west", "north", "back", "guide", "start", "left_stick",
                                "right_stick", "left_shoulder", "right_shoulder", "dpad_up", "dpad_down",
                                "dpad_left", "dpad_right"});
    CHECK(rv::input::gamepadButtonName(GamepadButton::South) == "south");
    CHECK(rv::input::gamepadButtonName(GamepadButton::North) == "north");
    CHECK(rv::input::gamepadButtonName(GamepadButton::Back) == "back");
    CHECK(rv::input::gamepadButtonName(GamepadButton::Start) == "start");
    CHECK(rv::input::gamepadButtonName(GamepadButton::LeftStick) == "left_stick");
    // Positional names only: vendor labels are not codes.
    CHECK_FALSE(rv::input::gamepadButtonFromName("a").has_value());
    CHECK_FALSE(rv::input::gamepadButtonFromName("cross").has_value());

    CHECK(rv::input::kGamepadAxisCount == 6);
    checkFamily<GamepadAxis>(rv::input::kGamepadAxisCount, rv::input::gamepadAxisName,
                             rv::input::gamepadAxisFromName,
                             {"left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger"});
    CHECK(rv::input::gamepadAxisName(GamepadAxis::LeftY) == "left_y");
    CHECK(rv::input::gamepadAxisName(GamepadAxis::RightTrigger) == "right_trigger");
}

TEST_CASE("input/codes: indices are dense and match the enum order", "[input][codes]") {
    CHECK(rv::input::toIndex(Key::A) == 0);
    CHECK(rv::input::toIndex(Key::Equals) == rv::input::kKeyCount - 1);
    CHECK(rv::input::toIndex(MouseButton::X2) == rv::input::kMouseButtonCount - 1);
    CHECK(rv::input::toIndex(MouseAxis::Wheel) == rv::input::kMouseAxisCount - 1);
    // Same order as SDL_GamepadButton / SDL_GamepadAxis, which keeps the SDL translation trivial to audit.
    CHECK(rv::input::toIndex(GamepadButton::South) == 0);
    CHECK(rv::input::toIndex(GamepadButton::Start) == 6);
    CHECK(rv::input::toIndex(GamepadButton::DpadRight) == rv::input::kGamepadButtonCount - 1);
    CHECK(rv::input::toIndex(GamepadAxis::LeftX) == 0);
    CHECK(rv::input::toIndex(GamepadAxis::RightTrigger) == rv::input::kGamepadAxisCount - 1);
}
