#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "support/temp_dir.hpp"
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/binding.hpp>

using rv::ErrorCode;
using rv::input::Action;
using rv::input::ActionMap;
using rv::input::Binding;
using rv::input::GamepadAxis;
using rv::input::GamepadAxisBinding;
using rv::input::GamepadButton;
using rv::input::GamepadButtonBinding;
using rv::input::Key;
using rv::input::KeyBinding;
using rv::input::MouseAxis;
using rv::input::MouseAxisBinding;
using rv::input::MouseButton;
using rv::input::MouseButtonBinding;
using Catch::Matchers::ContainsSubstring;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfinity = std::numeric_limits<float>::infinity();

std::vector<Binding> bindingsOf(const ActionMap& map, Action action) {
    const std::span<const Binding> bindings = map.bindings(action);
    return {bindings.begin(), bindings.end()};
}

/// Readable form for failure messages.
std::string describe(std::span<const Binding> bindings) {
    std::ostringstream out;
    for (const Binding& binding : bindings) {
        std::visit(
            [&out](const auto& b) {
                using T = std::decay_t<decltype(b)>;
                if constexpr (std::is_same_v<T, KeyBinding>) {
                    out << "key " << rv::input::keyName(b.key);
                } else if constexpr (std::is_same_v<T, MouseButtonBinding>) {
                    out << "mouseButton " << rv::input::mouseButtonName(b.button);
                } else if constexpr (std::is_same_v<T, MouseAxisBinding>) {
                    out << "mouseAxis " << rv::input::mouseAxisName(b.axis);
                } else if constexpr (std::is_same_v<T, GamepadButtonBinding>) {
                    out << "padButton " << rv::input::gamepadButtonName(b.button);
                } else {
                    out << "padAxis " << rv::input::gamepadAxisName(b.axis) << " deadzone " << b.deadzone
                        << " exponent " << b.exponent;
                }
                out << " scale " << b.scale << "; ";
            },
            binding);
    }
    return out.str();
}

rv::Expected<ActionMap> parseMap(std::string_view text, std::string_view context = "test.json") {
    const auto document = rv::core::parseJson(text, context);
    REQUIRE(document.has_value());
    return ActionMap::fromJson(*document, context);
}

rv::Error parseFailure(std::string_view text, std::string_view context = "test.json") {
    const auto map = parseMap(text, context);
    REQUIRE_FALSE(map.has_value());
    return map.error();
}

/// Wraps one binding entry into a complete document under "move_forward".
std::string withEntry(std::string_view entry) {
    return R"({"schemaVersion": 1, "bindings": {"move_forward": [)" + std::string(entry) + "]}}";
}

} // namespace

TEST_CASE("input/binding: validity of codes and parameters", "[input][binding]") {
    CHECK(rv::input::isValid(KeyBinding{Key::E, 1.0f}));
    CHECK(rv::input::isValid(KeyBinding{Key::Equals, -3.0f}));
    CHECK(rv::input::isValid(KeyBinding{Key::Space, 0.0f}));
    CHECK(rv::input::isValid(MouseButtonBinding{MouseButton::X2, 1.0f}));
    CHECK(rv::input::isValid(MouseAxisBinding{MouseAxis::Wheel, -0.5f}));
    CHECK(rv::input::isValid(GamepadButtonBinding{GamepadButton::DpadRight, 1.0f}));
    CHECK(rv::input::isValid(GamepadAxisBinding{GamepadAxis::RightTrigger, 1.0f, 0.0f, 0.5f}));
    CHECK(rv::input::isValid(GamepadAxisBinding{GamepadAxis::LeftY, 1.0f, 0.99f, 3.0f}));

    CHECK_FALSE(rv::input::isValid(KeyBinding{Key::Count, 1.0f}));
    CHECK_FALSE(rv::input::isValid(KeyBinding{Key::E, kNaN}));
    CHECK_FALSE(rv::input::isValid(KeyBinding{Key::E, kInfinity}));
    CHECK_FALSE(rv::input::isValid(MouseButtonBinding{MouseButton::Count, 1.0f}));
    CHECK_FALSE(rv::input::isValid(MouseAxisBinding{MouseAxis::Count, 1.0f}));
    CHECK_FALSE(rv::input::isValid(MouseAxisBinding{MouseAxis::DeltaX, -kInfinity}));
    CHECK_FALSE(rv::input::isValid(GamepadButtonBinding{GamepadButton::Count, 1.0f}));
    CHECK_FALSE(rv::input::isValid(GamepadAxisBinding{GamepadAxis::Count, 1.0f}));
    CHECK_FALSE(rv::input::isValid(GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, 1.0f, 1.0f}));
    CHECK_FALSE(rv::input::isValid(GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, -0.1f, 1.0f}));
    CHECK_FALSE(rv::input::isValid(GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, kNaN, 1.0f}));
    CHECK_FALSE(rv::input::isValid(GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, 0.1f, 0.0f}));
    CHECK_FALSE(rv::input::isValid(GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, 0.1f, -2.0f}));
    CHECK_FALSE(rv::input::isValid(GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, 0.1f, kInfinity}));

    // Defaults of a gamepad axis binding.
    const GamepadAxisBinding axis{GamepadAxis::LeftY, 1.0f};
    CHECK(axis.deadzone == 0.15f);
    CHECK(axis.exponent == 1.0f);
}

TEST_CASE("input/action_map: bind, clear and lookup", "[input][action_map]") {
    ActionMap map;
    for (std::size_t i = 0; i < rv::input::kActionCount; ++i) {
        CHECK(map.bindings(static_cast<Action>(i)).empty());
    }

    map.bind(Action::Interact, KeyBinding{Key::E, 1.0f});
    map.bind(Action::Interact, MouseButtonBinding{MouseButton::Left, 1.0f});
    map.bind(Action::Interact, KeyBinding{Key::E, 1.0f}); // identical: ignored, never doubles a contribution
    map.bind(Action::Interact, KeyBinding{Key::E, 0.5f}); // different scale: a distinct binding
    CHECK(bindingsOf(map, Action::Interact) ==
          std::vector<Binding>{KeyBinding{Key::E, 1.0f}, MouseButtonBinding{MouseButton::Left, 1.0f},
                               KeyBinding{Key::E, 0.5f}});
    CHECK(map.bindings(Action::Back).empty());

    // Invalid bindings and out-of-range actions are ignored without crashing.
    map.bind(Action::Back, KeyBinding{Key::Count, 1.0f});
    map.bind(Action::Back, KeyBinding{Key::Escape, kNaN});
    map.bind(Action::Back, GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, 1.0f, 1.0f});
    map.bind(Action::Back, GamepadAxisBinding{GamepadAxis::LeftX, 1.0f, 0.1f, 0.0f});
    CHECK(map.bindings(Action::Back).empty());
    map.bind(Action::Count, KeyBinding{Key::E, 1.0f});
    CHECK(map.bindings(Action::Count).empty());
    map.clear(Action::Count);

    // An action holds at most kMaxBindingsPerAction bindings; the first ones are kept.
    ActionMap crowded;
    for (std::size_t i = 0; i < rv::input::kKeyCount; ++i) {
        crowded.bind(Action::Screenshot, KeyBinding{static_cast<Key>(i), 1.0f});
    }
    const std::vector<Binding> kept = bindingsOf(crowded, Action::Screenshot);
    REQUIRE(kept.size() == ActionMap::kMaxBindingsPerAction);
    CHECK(kept.front() == Binding{KeyBinding{Key::A, 1.0f}});
    CHECK(kept.back() == Binding{KeyBinding{static_cast<Key>(ActionMap::kMaxBindingsPerAction - 1), 1.0f}});

    // Equality compares bindings in order.
    ActionMap reordered;
    reordered.bind(Action::Interact, MouseButtonBinding{MouseButton::Left, 1.0f});
    reordered.bind(Action::Interact, KeyBinding{Key::E, 1.0f});
    reordered.bind(Action::Interact, KeyBinding{Key::E, 0.5f});
    CHECK_FALSE(reordered == map);

    const ActionMap copy = map;
    CHECK(copy == map);
    map.clear(Action::Interact);
    CHECK(map.bindings(Action::Interact).empty());
    CHECK_FALSE(copy == map);
    CHECK(map == ActionMap{});
}

TEST_CASE("input/action_map: defaults implement the specification controls table", "[input][action_map]") {
    const auto key = [](Key k, float scale = 1.0f) { return Binding{KeyBinding{k, scale}}; };
    const auto mouseButton = [](MouseButton b) { return Binding{MouseButtonBinding{b, 1.0f}}; };
    const auto mouseAxis = [](MouseAxis a, float scale) { return Binding{MouseAxisBinding{a, scale}}; };
    const auto padButton = [](GamepadButton b) { return Binding{GamepadButtonBinding{b, 1.0f}}; };
    const auto padAxis = [](GamepadAxis a) { return Binding{GamepadAxisBinding{a, 1.0f, 0.15f, 1.0f}}; };
    // A stick read whole: no dead zone (the UI's hysteresis is its dead zone, ADR-0005).
    const auto wholeAxis = [](GamepadAxis a) {
        return Binding{GamepadAxisBinding{a, 1.0f, 0.0f, 1.0f}};
    };

    // Specification §12.1, written out independently of the implementation.
    const std::vector<std::pair<Action, std::vector<Binding>>> expected = {
        {Action::MoveForward,
         {key(Key::W), key(Key::S, -1.0f), key(Key::Up), key(Key::Down, -1.0f), padAxis(GamepadAxis::LeftY)}},
        {Action::MoveRight,
         {key(Key::D), key(Key::A, -1.0f), key(Key::Right), key(Key::Left, -1.0f), padAxis(GamepadAxis::LeftX)}},
        {Action::LookX, {padAxis(GamepadAxis::RightX)}},
        {Action::LookY, {padAxis(GamepadAxis::RightY)}},
        {Action::LookDeltaX, {mouseAxis(MouseAxis::DeltaX, 1.0f)}},
        {Action::LookDeltaY, {mouseAxis(MouseAxis::DeltaY, -1.0f)}},
        {Action::Sprint, {key(Key::LeftShift), padButton(GamepadButton::LeftStick)}},
        {Action::Interact, {key(Key::E), mouseButton(MouseButton::Left), padButton(GamepadButton::South)}},
        {Action::Back, {key(Key::Escape), padButton(GamepadButton::East)}},
        {Action::QuickCatalog, {key(Key::Tab), padButton(GamepadButton::North)}},
        {Action::Confirm, {key(Key::Enter), padButton(GamepadButton::Start)}},
        {Action::Map, {key(Key::M), padButton(GamepadButton::Back)}},
        {Action::Menu, {padButton(GamepadButton::Start)}},
        {Action::ToggleDevUI, {key(Key::F1)}},
        {Action::ReloadShaders, {key(Key::F5)}},
        {Action::Screenshot, {key(Key::F12)}},
        // The product UI (ADR-0005, I2 step 6): arrows and D-pad, the left stick read whole (NavInput's hysteresis of
        // 0.55 / 0.35 of its travel is its dead zone), the pad's buttons of the I1 spike, the triggers page (with the
        // usual dead zone), the stick clicks jump, the shoulders change the filter.
        {Action::UiUp, {key(Key::Up), padButton(GamepadButton::DpadUp)}},
        {Action::UiDown, {key(Key::Down), padButton(GamepadButton::DpadDown)}},
        {Action::UiLeft, {key(Key::Left), padButton(GamepadButton::DpadLeft)}},
        {Action::UiRight, {key(Key::Right), padButton(GamepadButton::DpadRight)}},
        {Action::UiNavX, {wholeAxis(GamepadAxis::LeftX)}},
        {Action::UiNavY, {wholeAxis(GamepadAxis::LeftY)}},
        {Action::UiPageUp, {key(Key::PageUp), padAxis(GamepadAxis::LeftTrigger)}},
        {Action::UiPageDown, {key(Key::PageDown), padAxis(GamepadAxis::RightTrigger)}},
        {Action::UiHome, {key(Key::Home), padButton(GamepadButton::LeftStick)}},
        {Action::UiEnd, {key(Key::End), padButton(GamepadButton::RightStick)}},
        {Action::UiActivate, {key(Key::Enter), padButton(GamepadButton::South)}},
        {Action::UiBack, {key(Key::Escape), padButton(GamepadButton::East)}},
        {Action::UiMedia, {padButton(GamepadButton::North)}},
        {Action::UiInfo, {padButton(GamepadButton::West)}},
        {Action::UiFilterPrevious, {padButton(GamepadButton::LeftShoulder)}},
        {Action::UiFilterNext, {padButton(GamepadButton::RightShoulder)}},
        // The editor (I4b step 3, ADR-0015 §4).
        {Action::EditPin, {key(Key::P), padButton(GamepadButton::West)}},
    };
    REQUIRE(expected.size() == rv::input::kActionCount);

    const ActionMap map = ActionMap::defaults();
    for (const auto& [action, bindings] : expected) {
        INFO(rv::input::actionName(action) << ": " << describe(map.bindings(action)));
        CHECK(bindingsOf(map, action) == bindings);
    }
}

TEST_CASE("input/action_map: default_bindings.json is equivalent to defaults()", "[input][action_map]") {
    const auto path = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR "/config/default_bindings.json");
    const auto loaded = ActionMap::loadFromFile(path);
    if (!loaded) {
        FAIL(loaded.error().toString());
    }
    const ActionMap defaults = ActionMap::defaults();
    for (std::size_t i = 0; i < rv::input::kActionCount; ++i) {
        const auto action = static_cast<Action>(i);
        INFO(rv::input::actionName(action) << " file: " << describe(loaded->bindings(action))
                                           << " code: " << describe(defaults.bindings(action)));
        CHECK(bindingsOf(*loaded, action) == bindingsOf(defaults, action));
    }
    CHECK(*loaded == defaults);
}

TEST_CASE("input/action_map: JSON round trip", "[input][action_map]") {
    constexpr float kFloatMax = std::numeric_limits<float>::max();
    ActionMap custom;
    custom.bind(Action::MoveForward, KeyBinding{Key::Digit0, 0.1f});
    custom.bind(Action::MoveForward, KeyBinding{Key::Grave, -1.0f});
    custom.bind(Action::LookX, GamepadAxisBinding{GamepadAxis::RightTrigger, -2.5f, 0.2f, 1.75f});
    custom.bind(Action::LookDeltaY, MouseAxisBinding{MouseAxis::Wheel, 1.0f / 3.0f});
    custom.bind(Action::Interact, MouseButtonBinding{MouseButton::X2, 0.35f});
    custom.bind(Action::Menu, GamepadButtonBinding{GamepadButton::DpadRight, -1.0f});
    custom.bind(Action::Sprint, GamepadAxisBinding{GamepadAxis::LeftTrigger, 1.0f, 0.0f, 0.3f});
    // Extreme finite values: FLT_MAX is written as 3.4028235e+38, slightly above FLT_MAX once read as a double.
    custom.bind(Action::MoveRight, KeyBinding{Key::D, kFloatMax});
    custom.bind(Action::MoveRight, KeyBinding{Key::A, -kFloatMax});
    custom.bind(Action::LookY, GamepadAxisBinding{GamepadAxis::RightY, -kFloatMax, 0.0f, kFloatMax});

    for (const ActionMap& map : {ActionMap::defaults(), custom, ActionMap{}}) {
        const nlohmann::json json = map.toJson();
        const auto restored = ActionMap::fromJson(json, "memory");
        REQUIRE(restored.has_value());
        CHECK(*restored == map);

        // Through text, as a file written to disk and read back.
        const auto text = rv::core::parseJson(json.dump(2), "text");
        REQUIRE(text.has_value());
        const auto reread = ActionMap::fromJson(*text, "text");
        REQUIRE(reread.has_value());
        CHECK(*reread == map);
        CHECK(reread->toJson() == json);
    }

    // Numbers are range-checked once rounded to float, so the shortest form of FLT_MAX is accepted when written
    // by hand too (the first decimal that rounds to infinity is an error case below).
    const auto extreme = parseMap(withEntry(R"({"key": "w", "scale": 3.4028235e38},
                                               {"key": "s", "scale": -3.4028235e38})"));
    REQUIRE(extreme.has_value());
    CHECK(bindingsOf(*extreme, Action::MoveForward) ==
          std::vector<Binding>{KeyBinding{Key::W, kFloatMax}, KeyBinding{Key::S, -kFloatMax}});
}

TEST_CASE("input/action_map: toJson writes every action and every field", "[input][action_map]") {
    const nlohmann::json json = ActionMap::defaults().toJson();
    CHECK(json.size() == 2);
    CHECK(json.at("schemaVersion") == 1);
    const nlohmann::json& bindings = json.at("bindings");
    CHECK(bindings.size() == rv::input::kActionCount);

    const nlohmann::json& forward = bindings.at("move_forward");
    REQUIRE(forward.size() == 5);
    CHECK(forward[0] == nlohmann::json::parse(R"({"key": "w", "scale": 1.0})"));
    CHECK(forward[1] == nlohmann::json::parse(R"({"key": "s", "scale": -1.0})"));
    CHECK(forward[4] == nlohmann::json::parse(R"({"padAxis": "left_y", "scale": 1.0, "deadzone": 0.15,
                                                   "exponent": 1.0})"));
    // Shortest exact decimal form, not the binary expansion of 0.15f.
    CHECK(forward[4].at("deadzone").dump() == "0.15");
    CHECK(bindings.at("look_delta_y")[0] == nlohmann::json::parse(R"({"mouseAxis": "delta_y", "scale": -1.0})"));
    CHECK(bindings.at("interact")[1] == nlohmann::json::parse(R"({"mouseButton": "left", "scale": 1.0})"));
    CHECK(bindings.at("menu")[0] == nlohmann::json::parse(R"({"padButton": "start", "scale": 1.0})"));

    // Unbound actions are written as empty arrays.
    ActionMap sparse;
    sparse.bind(Action::Screenshot, KeyBinding{Key::F12, 1.0f});
    const nlohmann::json sparseJson = sparse.toJson();
    CHECK(sparseJson.at("bindings").size() == rv::input::kActionCount);
    CHECK(sparseJson.at("bindings").at("move_forward") == nlohmann::json::array());
    CHECK(sparseJson.at("bindings").at("screenshot").size() == 1);
}

TEST_CASE("input/action_map: optional fields take their defaults", "[input][action_map]") {
    const auto map = parseMap(R"({
        // Comments are accepted in hand-written binding files.
        "schemaVersion": 1,
        "bindings": {
            "interact": [{"key": "e"}, {"key": "e"}],
            "move_forward": [{"padAxis": "left_y"},
                             {"padAxis": "right_y", "scale": -0.5, "deadzone": 0, "exponent": 2}],
            "look_delta_x": [{"mouseAxis": "delta_x", "scale": 2}],
            "sprint": []
        }
    })");
    REQUIRE(map.has_value());
    // Exact duplicates collapse into one binding.
    CHECK(bindingsOf(*map, Action::Interact) == std::vector<Binding>{KeyBinding{Key::E, 1.0f}});
    CHECK(bindingsOf(*map, Action::MoveForward) ==
          std::vector<Binding>{GamepadAxisBinding{GamepadAxis::LeftY, 1.0f, 0.15f, 1.0f},
                               GamepadAxisBinding{GamepadAxis::RightY, -0.5f, 0.0f, 2.0f}});
    CHECK(bindingsOf(*map, Action::LookDeltaX) == std::vector<Binding>{MouseAxisBinding{MouseAxis::DeltaX, 2.0f}});
    CHECK(map->bindings(Action::Sprint).empty());
    // Actions missing from the document have no bindings.
    CHECK(map->bindings(Action::Menu).empty());
    CHECK(map->bindings(Action::LookX).empty());

    const auto empty = parseMap(R"({"schemaVersion": 1, "bindings": {}})");
    REQUIRE(empty.has_value());
    CHECK(*empty == ActionMap{});
}

TEST_CASE("input/action_map: errors name the failure and its JSON path", "[input][action_map]") {
    struct ErrorCase {
        std::string_view what;
        std::string text;
        ErrorCode code;
        std::string_view context;
        std::string_view messageFragment;
    };
    std::string tooManyEntries = R"({"key": "w"})";
    for (std::size_t i = 0; i < ActionMap::kMaxBindingsPerAction; ++i) {
        tooManyEntries += R"(, {"key": "w"})";
    }
    const std::vector<ErrorCase> cases = {
        {"root is not an object", "[1, 2]", ErrorCode::ParseError, "test.json", "object"},
        {"unknown top-level field", R"({"schemaVersion": 1, "bindings": {}, "extra": true})", ErrorCode::ParseError,
         "test.json:/extra", "unknown field \"extra\""},
        {"missing schemaVersion", R"({"bindings": {}})", ErrorCode::ParseError, "test.json:/schemaVersion",
         "schemaVersion"},
        {"schemaVersion of the wrong type", R"({"schemaVersion": "1", "bindings": {}})", ErrorCode::ParseError,
         "test.json:/schemaVersion", "integer"},
        {"schemaVersion mismatch", R"({"schemaVersion": 2, "bindings": {}})", ErrorCode::Unsupported,
         "test.json:/schemaVersion", "2"},
        {"newer schemaVersion with new fields", R"({"schemaVersion": 2, "bindings": {}, "profiles": {}})",
         ErrorCode::Unsupported, "test.json:/schemaVersion", "2"},
        {"missing bindings", R"({"schemaVersion": 1})", ErrorCode::ParseError, "test.json:/bindings", "bindings"},
        {"bindings of the wrong type", R"({"schemaVersion": 1, "bindings": []})", ErrorCode::ParseError,
         "test.json:/bindings", "object"},
        {"unknown action", R"({"schemaVersion": 1, "bindings": {"teleport": []}})", ErrorCode::ValidationFailed,
         "test.json:/bindings/teleport", "teleport"},
        {"binding list of the wrong type", R"({"schemaVersion": 1, "bindings": {"move_forward": {"key": "w"}}})",
         ErrorCode::ParseError, "test.json:/bindings/move_forward", "array"},
        {"binding entry of the wrong type", withEntry(R"("w")"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0", "object"},
        {"unknown key name", withEntry(R"({"key": "w"}, {"key": "kw"})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/1/key", "\"kw\""},
        {"key names are case-sensitive", withEntry(R"({"key": "W"})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/0/key", "lowercase"},
        {"key name of the wrong type", withEntry(R"({"key": 87})"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0/key", "string"},
        {"unknown mouse button", withEntry(R"({"mouseButton": "x3"})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/0/mouseButton", "\"x3\""},
        {"unknown mouse axis", withEntry(R"({"mouseAxis": "delta_z"})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/0/mouseAxis", "\"delta_z\""},
        {"unknown gamepad button", withEntry(R"({"padButton": "a"})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/0/padButton", "\"a\""},
        {"unknown gamepad axis", withEntry(R"({"padAxis": "left_z"})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/0/padAxis", "\"left_z\""},
        {"two device fields", withEntry(R"({"key": "w", "padAxis": "left_y"})"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0", "two device fields"},
        {"no device field", withEntry(R"({"scale": 1})"), ErrorCode::ParseError, "test.json:/bindings/move_forward/0",
         "device field"},
        {"misspelled device field", withEntry(R"({"kee": "w"})"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0/kee", "unknown field \"kee\""},
        {"too many bindings", withEntry(tooManyEntries), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward", "too many bindings"},
        {"unknown field in a binding", withEntry(R"({"key": "w", "scal": 1})"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0/scal", "unknown field \"scal\""},
        {"deadzone outside padAxis", withEntry(R"({"key": "w", "deadzone": 0.2})"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0/deadzone", "padAxis"},
        {"exponent outside padAxis", withEntry(R"({"padButton": "south", "exponent": 2})"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0/exponent", "padAxis"},
        {"scale of the wrong type", withEntry(R"({"key": "w", "scale": "1"})"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0/scale", "number"},
        {"scale is a boolean", withEntry(R"({"key": "w", "scale": true})"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0/scale", "number"},
        {"scale overflows a float", withEntry(R"({"key": "w", "scale": 1e39})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/0/scale", "range"},
        {"scale rounds to infinity as a float", withEntry(R"({"key": "w", "scale": -3.4028236e38})"),
         ErrorCode::ValidationFailed, "test.json:/bindings/move_forward/0/scale", "range"},
        {"deadzone of 1", withEntry(R"({"padAxis": "left_y", "deadzone": 1})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/0/deadzone", "[0, 1)"},
        {"negative deadzone", withEntry(R"({"padAxis": "left_y", "deadzone": -0.1})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/0/deadzone", "[0, 1)"},
        {"exponent of 0", withEntry(R"({"padAxis": "left_y", "exponent": 0})"), ErrorCode::ValidationFailed,
         "test.json:/bindings/move_forward/0/exponent", "greater than 0"},
        {"pointer tokens are escaped", withEntry(R"({"key": "w", "a/b~c": 1})"), ErrorCode::ParseError,
         "test.json:/bindings/move_forward/0/a~1b~0c", "a/b~c"},
    };

    for (const ErrorCase& c : cases) {
        INFO(c.what << ": " << c.text);
        const rv::Error error = parseFailure(c.text);
        CHECK(error.code == c.code);
        CHECK(error.context == c.context);
        CHECK_THAT(error.message, ContainsSubstring(std::string(c.messageFragment)));
    }

    // The context names the document as given by the caller.
    const rv::Error named = parseFailure(withEntry(R"({"key": "w"}, {"key": "kw"})"), "default_bindings.json");
    CHECK(named.context == "default_bindings.json:/bindings/move_forward/1/key");

    // In-memory documents can hold numbers that JSON text cannot: infinity and NaN are out of range.
    for (const double bad : {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        nlohmann::json document = nlohmann::json::parse(withEntry(R"({"padAxis": "left_y"})"));
        document["bindings"]["move_forward"][0]["exponent"] = bad;
        const auto map = ActionMap::fromJson(document, "memory");
        REQUIRE_FALSE(map.has_value());
        CHECK(map.error().code == ErrorCode::ValidationFailed);
        CHECK(map.error().context == "memory:/bindings/move_forward/0/exponent");
    }
}

TEST_CASE("input/action_map: loadFromFile reports the file path", "[input][action_map]") {
    rv::test::TempDir dir;

    const auto good = dir.file("bindings.json");
    REQUIRE(rv::core::writeTextFileAtomic(good, R"({"schemaVersion": 1, "bindings": {"interact": [{"key": "f"}]}})")
                .has_value());
    const auto loaded = ActionMap::loadFromFile(good);
    REQUIRE(loaded.has_value());
    CHECK(bindingsOf(*loaded, Action::Interact) == std::vector<Binding>{KeyBinding{Key::F, 1.0f}});

    const auto bad = dir.file("bad_bindings.json");
    REQUIRE(rv::core::writeTextFileAtomic(bad, R"({"schemaVersion": 1, "bindings": {"interact": [{"key": "?"}]}})")
                .has_value());
    const auto invalid = ActionMap::loadFromFile(bad);
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error().code == ErrorCode::ValidationFailed);
    CHECK(invalid.error().context == rv::core::pathToUtf8(bad) + ":/bindings/interact/0/key");

    const auto malformed = dir.file("malformed.json");
    REQUIRE(rv::core::writeTextFileAtomic(malformed, R"({"schemaVersion": 1, "bindings": )").has_value());
    const auto broken = ActionMap::loadFromFile(malformed);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == ErrorCode::ParseError);
    CHECK(broken.error().context == rv::core::pathToUtf8(malformed));

    // A number that overflows a double is a parse error too, not an exception leaving the module.
    const auto overflow = dir.file("overflow.json");
    REQUIRE(rv::core::writeTextFileAtomic(overflow, withEntry(R"({"key": "w", "scale": 1e400})")).has_value());
    const auto overflowed = ActionMap::loadFromFile(overflow);
    REQUIRE_FALSE(overflowed.has_value());
    CHECK(overflowed.error().code == ErrorCode::ParseError);
    CHECK(overflowed.error().context == rv::core::pathToUtf8(overflow));
    CHECK_THAT(overflowed.error().message, ContainsSubstring("1e400"));

    const auto missing = ActionMap::loadFromFile(dir.file("missing.json"));
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == ErrorCode::NotFound);
}

TEST_CASE("input/action_map: user overrides replace only the actions they list", "[input][action_map]") {
    ActionMap map = ActionMap::defaults();
    const auto document = rv::core::parseJson(R"({
        "schemaVersion": 1,
        "bindings": {
            "move_forward": [{"key": "i"}, {"key": "k", "scale": -1}],
            "interact": []
        }
    })",
                                              "user.json");
    REQUIRE(document.has_value());
    const auto replaced = map.applyOverrides(*document, "user.json");
    REQUIRE(replaced.has_value());
    CHECK(*replaced == 2);
    CHECK(bindingsOf(map, Action::MoveForward) ==
          std::vector<Binding>{KeyBinding{Key::I, 1.0f}, KeyBinding{Key::K, -1.0f}});
    CHECK(map.bindings(Action::Interact).empty()); // an empty list unbinds
    // Everything else keeps the defaults.
    const ActionMap defaults = ActionMap::defaults();
    for (const Action action : {Action::MoveRight, Action::LookDeltaX, Action::Back, Action::Screenshot}) {
        INFO(rv::input::actionName(action));
        CHECK(bindingsOf(map, action) == bindingsOf(defaults, action));
    }

    // A full document (toJson) replaces everything; an invalid one changes nothing.
    ActionMap full = ActionMap::defaults();
    ActionMap custom;
    custom.bind(Action::Back, KeyBinding{Key::Backspace, 1.0f});
    const auto all = full.applyOverrides(custom.toJson(), "custom.json");
    REQUIRE(all.has_value());
    CHECK(*all == rv::input::kActionCount);
    CHECK(full == custom);

    ActionMap untouched = ActionMap::defaults();
    const auto invalid = rv::core::parseJson(
        R"({"schemaVersion": 1, "bindings": {"move_forward": [{"key": "i"}], "back": [{"key": "nope"}]}})", "bad.json");
    REQUIRE(invalid.has_value());
    const auto rejected = untouched.applyOverrides(*invalid, "bad.json");
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == ErrorCode::ValidationFailed);
    CHECK(rejected.error().context == "bad.json:/bindings/back/0/key");
    CHECK(untouched == ActionMap::defaults());
}

TEST_CASE("input/action_map: user overrides rebind the UI's actions apart from the world's", "[input][action_map]") {
    // A user file (<user data>/config/bindings.json) over the shipped defaults, as app::loadBindings applies it.
    auto map = ActionMap::loadFromFile(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR "/config/default_bindings.json"));
    REQUIRE(map.has_value());
    const auto document = rv::core::parseJson(R"({
        // comments are allowed, as in every configuration file
        "schemaVersion": 1,
        "bindings": {
            "ui_back": [{"key": "backspace"}, {"padButton": "east"}],
            "ui_nav_y": [{"padAxis": "right_y", "deadzone": 0.25}],
            "ui_media": []
        }
    })",
                                              "bindings.json");
    REQUIRE(document.has_value());
    const auto replaced = map->applyOverrides(*document, "bindings.json");
    REQUIRE(replaced.has_value());
    CHECK(*replaced == 3);
    CHECK(bindingsOf(*map, Action::UiBack) ==
          std::vector<Binding>{KeyBinding{Key::Backspace, 1.0f}, GamepadButtonBinding{GamepadButton::East, 1.0f}});
    CHECK(bindingsOf(*map, Action::UiNavY) ==
          std::vector<Binding>{GamepadAxisBinding{GamepadAxis::RightY, 1.0f, 0.25f, 1.0f}});
    CHECK(map->bindings(Action::UiMedia).empty());
    // The world's Back keeps Esc, the UI's other actions their defaults.
    const ActionMap defaults = ActionMap::defaults();
    for (const Action action : {Action::Back, Action::QuickCatalog, Action::UiUp, Action::UiNavX, Action::UiActivate}) {
        INFO(rv::input::actionName(action));
        CHECK(bindingsOf(*map, action) == bindingsOf(defaults, action));
    }
    // An unknown UI action is refused like any other.
    const auto typo = rv::core::parseJson(R"({"schemaVersion": 1, "bindings": {"ui_dwon": []}})", "typo.json");
    REQUIRE(typo.has_value());
    const auto refused = map->applyOverrides(*typo, "typo.json");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().context == "typo.json:/bindings/ui_dwon");
}
