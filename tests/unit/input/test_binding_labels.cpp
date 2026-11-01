// The keys' names (ADR-0018 §7): assets/ui/key_labels.json names every key and button of codes.hpp; what a hint names for
// an action on a device; a bad table refused.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/binding_labels.hpp>
#include <rv/input/codes.hpp>

using namespace rv::input;
using nlohmann::json;

namespace {

json shippedDocument() {
    auto document = rv::core::readJsonFile(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR "/ui/key_labels.json"));
    REQUIRE(document.has_value());
    return *document;
}

KeyLabels shippedLabels() {
    auto labels = KeyLabels::fromJson(shippedDocument(), "key_labels.json");
    REQUIRE(labels.has_value());
    return *labels;
}

} // namespace

TEST_CASE("input/binding_labels: the table names every key and button of codes.hpp", "[input][labels]") {
    const json document = shippedDocument();
    const KeyLabels labels = shippedLabels();
    for (std::size_t index = 0; index < kKeyCount; ++index) {
        const auto key = static_cast<Key>(index);
        INFO(keyName(key));
        CHECK(document["keys"].contains(std::string(keyName(key))));
        CHECK_FALSE(labels.label(KeyBinding{key, 1.0f}).empty());
    }
    for (std::size_t index = 0; index < kMouseButtonCount; ++index) {
        const auto button = static_cast<MouseButton>(index);
        INFO(mouseButtonName(button));
        CHECK(document["mouseButtons"].contains(std::string(mouseButtonName(button))));
    }
    for (std::size_t index = 0; index < kGamepadButtonCount; ++index) {
        const auto button = static_cast<GamepadButton>(index);
        INFO(gamepadButtonName(button));
        CHECK(document["padButtons"].contains(std::string(gamepadButtonName(button))));
    }
    for (std::size_t index = 0; index < kGamepadAxisCount; ++index) {
        const auto axis = static_cast<GamepadAxis>(index);
        INFO(gamepadAxisName(axis));
        CHECK(document["padAxes"].contains(std::string(gamepadAxisName(axis))));
    }
    CHECK(labels.label(KeyBinding{Key::Enter, 1.0f}) == "Invio");
    CHECK(labels.label(GamepadButtonBinding{GamepadButton::West, 1.0f}) == "X");
    CHECK(labels.label(GamepadAxisBinding{GamepadAxis::LeftTrigger}) == "LT");
    CHECK(labels.group("up down left right") == "Frecce");
    CHECK(labels.group("up left").empty());
}

TEST_CASE("input/binding_labels: an action's name on a device", "[input][labels]") {
    const KeyLabels labels = shippedLabels();
    const ActionMap defaults = ActionMap::defaults();
    CHECK(bindingLabel(defaults, Action::UiActivate, DeviceKind::KeyboardMouse, labels) == "Invio");
    CHECK(bindingLabel(defaults, Action::UiActivate, DeviceKind::Gamepad, labels) == "A");
    CHECK(bindingLabel(defaults, Action::Interact, DeviceKind::KeyboardMouse, labels) == "E"); // the key before the click
    CHECK(bindingLabel(defaults, Action::UiPageUp, DeviceKind::Gamepad, labels) == "LT");      // a trigger is an axis
    // Nothing of the device bound: no name.
    CHECK(bindingLabel(defaults, Action::ToggleMute, DeviceKind::Gamepad, labels).empty());
    // A click alone is named.
    ActionMap click;
    click.bind(Action::Interact, MouseButtonBinding{MouseButton::Left, 1.0f});
    CHECK(bindingLabel(click, Action::Interact, DeviceKind::KeyboardMouse, labels) == "Clic");
}

TEST_CASE("input/binding_labels: without the table a code shows its own name", "[input][labels]") {
    const KeyLabels empty;
    CHECK(empty.label(KeyBinding{Key::PageUp, 1.0f}) == "PAGE_UP");
    CHECK(empty.label(GamepadButtonBinding{GamepadButton::South, 1.0f}) == "SOUTH");
}

TEST_CASE("input/binding_labels: a bad table is refused", "[input][labels]") {
    CHECK_FALSE(KeyLabels::fromJson(json::array(), "t").has_value());
    CHECK_FALSE(KeyLabels::fromJson(json{{"schemaVersion", 2}}, "t").has_value());
    CHECK_FALSE(KeyLabels::fromJson(json{{"schemaVersion", 1}, {"stray", json::object()}}, "t").has_value());
    CHECK_FALSE(KeyLabels::fromJson(json{{"schemaVersion", 1}, {"keys", {{"no_such_key", "x"}}}}, "t").has_value());
    CHECK_FALSE(KeyLabels::fromJson(json{{"schemaVersion", 1}, {"padButtons", {{"south", 1}}}}, "t").has_value());
}

TEST_CASE("input/binding_labels: the same input, whatever its scale", "[input][labels]") {
    CHECK(sameInput(KeyBinding{Key::W, 1.0f}, KeyBinding{Key::W, -1.0f}));
    CHECK_FALSE(sameInput(KeyBinding{Key::W, 1.0f}, KeyBinding{Key::S, 1.0f}));
    CHECK(sameInput(GamepadAxisBinding{GamepadAxis::LeftTrigger, 1.0f, 0.15f},
                    GamepadAxisBinding{GamepadAxis::LeftTrigger, 1.0f, 0.3f}));
    // "left" names a key and a mouse button: different inputs.
    CHECK_FALSE(sameInput(KeyBinding{Key::Left, 1.0f}, MouseButtonBinding{MouseButton::Left, 1.0f}));
}
