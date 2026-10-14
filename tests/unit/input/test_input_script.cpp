#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>

#include "support/temp_dir.hpp"

using nlohmann::json;
using rv::ErrorCode;
using rv::input::Action;
using rv::input::ActionState;
using rv::input::InputScript;
using rv::input::InputScriptPlayer;

namespace {

constexpr float kThreshold = 0.5f;

InputScript parseOk(std::string_view text) {
    const json document = json::parse(text, nullptr, true, true);
    auto script = InputScript::fromJson(document, "script.json");
    if (!script) {
        FAIL(script.error().toString());
    }
    return std::move(*script);
}

rv::Error parseError(std::string_view text) {
    const json document = json::parse(text, nullptr, true, true);
    const auto script = InputScript::fromJson(document, "script.json");
    INFO(text);
    REQUIRE_FALSE(script.has_value());
    return script.error();
}

/// Plays `frames` frames of `script` over empty device input, like the application does without devices.
std::vector<ActionState> play(const InputScript& script, std::uint64_t frames) {
    rv::input::InputSystem system;
    InputScriptPlayer player(script);
    std::vector<ActionState> states;
    for (std::uint64_t frame = 0; frame < frames; ++frame) {
        ActionState state = system.resolve(rv::input::RawInputFrame{});
        player.apply(frame, state, system.settings().pressThreshold);
        states.push_back(state);
    }
    return states;
}

std::filesystem::path sourceDir() {
    return rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR);
}

} // namespace

TEST_CASE("input/input_script: steps cover inclusive frame ranges", "[input][input_script]") {
    const InputScript script = parseOk(R"([
        // comments are allowed
        {"fromFrame": 0, "toFrame": 119, "actions": {"move_forward": 1.0}},
        {"fromFrame": 120, "toFrame": 120, "actions": {"look_delta_x": -35.5, "sprint": 1, "move_right": -0.25}}
    ])");
    REQUIRE(script.steps().size() == 2);
    CHECK(script.steps()[0].fromFrame == 0);
    CHECK(script.steps()[0].toFrame == 119);
    CHECK(script.steps()[1].values.size() == 3);
    CHECK(script.lastFrame() == 120);

    CHECK(script.valueAt(0, Action::MoveForward) == 1.0f);
    CHECK(script.valueAt(119, Action::MoveForward) == 1.0f);
    CHECK_FALSE(script.valueAt(120, Action::MoveForward).has_value());
    CHECK(script.valueAt(120, Action::LookDeltaX) == -35.5f);
    CHECK(script.valueAt(120, Action::MoveRight) == -0.25f);
    CHECK_FALSE(script.valueAt(121, Action::Sprint).has_value());
    CHECK_FALSE(script.valueAt(5, Action::LookDeltaX).has_value());
}

TEST_CASE("input/input_script: the later step wins where steps overlap", "[input][input_script]") {
    const InputScript script = parseOk(R"([
        {"fromFrame": 0, "toFrame": 99, "actions": {"move_forward": 1.0, "move_right": 0.5}},
        {"fromFrame": 40, "toFrame": 49, "actions": {"move_forward": 0.0}}
    ])");
    CHECK(script.valueAt(39, Action::MoveForward) == 1.0f);
    CHECK(script.valueAt(40, Action::MoveForward) == 0.0f);
    CHECK(script.valueAt(49, Action::MoveForward) == 0.0f);
    CHECK(script.valueAt(50, Action::MoveForward) == 1.0f);
    CHECK(script.valueAt(45, Action::MoveRight) == 0.5f); // not overridden by the later step

    const std::vector<ActionState> states = play(script, 100);
    CHECK(states[39].axis(Action::MoveForward) == 1.0f);
    CHECK(states[45].axis(Action::MoveForward) == 0.0f);
    CHECK(states[45].axis(Action::MoveRight) == 0.5f);
    CHECK(states[50].axis(Action::MoveForward) == 1.0f);
}

TEST_CASE("input/input_script: errors name the failure and its JSON path", "[input][input_script]") {
    struct Case {
        std::string_view text;
        ErrorCode code;
        std::string_view context;
    };
    const Case cases[] = {
        {R"({"fromFrame": 0})", ErrorCode::ParseError, "script.json"},
        {R"([])", ErrorCode::ValidationFailed, "script.json"},
        {R"([3])", ErrorCode::ParseError, "script.json:/0"},
        {R"([{"toFrame": 1, "actions": {"back": 1}}])", ErrorCode::ParseError, "script.json:/0/fromFrame"},
        {R"([{"fromFrame": 0, "actions": {"back": 1}}])", ErrorCode::ParseError, "script.json:/0/toFrame"},
        {R"([{"fromFrame": 0, "toFrame": 1}])", ErrorCode::ParseError, "script.json:/0/actions"},
        {R"([{"fromFrame": 0, "toFrame": 1, "actions": {"back": 1}, "repeat": 2}])", ErrorCode::ParseError,
         "script.json:/0/repeat"},
        {R"([{"fromFrame": 1.5, "toFrame": 2, "actions": {"back": 1}}])", ErrorCode::ParseError,
         "script.json:/0/fromFrame"},
        {R"([{"fromFrame": "0", "toFrame": 2, "actions": {"back": 1}}])", ErrorCode::ParseError,
         "script.json:/0/fromFrame"},
        {R"([{"fromFrame": -1, "toFrame": 2, "actions": {"back": 1}}])", ErrorCode::ValidationFailed,
         "script.json:/0/fromFrame"},
        {R"([{"fromFrame": 0, "toFrame": 4294967296, "actions": {"back": 1}}])", ErrorCode::ValidationFailed,
         "script.json:/0/toFrame"},
        {R"([{"fromFrame": 10, "toFrame": 9, "actions": {"back": 1}}])", ErrorCode::ValidationFailed,
         "script.json:/0/toFrame"},
        {R"([{"fromFrame": 0, "toFrame": 1, "actions": []}])", ErrorCode::ParseError, "script.json:/0/actions"},
        {R"([{"fromFrame": 0, "toFrame": 1, "actions": {}}])", ErrorCode::ValidationFailed, "script.json:/0/actions"},
        {R"([{"fromFrame": 0, "toFrame": 1, "actions": {"jump": 1}}])", ErrorCode::ValidationFailed,
         "script.json:/0/actions/jump"},
        {R"([{"fromFrame": 0, "toFrame": 1, "actions": {"sprint": true}}])", ErrorCode::ParseError,
         "script.json:/0/actions/sprint"},
        {R"([{"fromFrame": 0, "toFrame": 1, "actions": {"move_forward": 1.5}}])", ErrorCode::ValidationFailed,
         "script.json:/0/actions/move_forward"},
        {R"([{"fromFrame": 0, "toFrame": 1, "actions": {"look_x": -2}}])", ErrorCode::ValidationFailed,
         "script.json:/0/actions/look_x"},
        {R"([{"fromFrame": 0, "toFrame": 1, "actions": {"look_delta_y": 1e39}}])", ErrorCode::ValidationFailed,
         "script.json:/0/actions/look_delta_y"},
        {R"([{"fromFrame": 0, "toFrame": 1, "actions": {"back": 1}},
             {"fromFrame": 0, "toFrame": 1, "actions": {"a/b~c": 1}}])",
         ErrorCode::ValidationFailed, "script.json:/1/actions/a~1b~0c"},
    };
    for (const Case& c : cases) {
        const rv::Error error = parseError(c.text);
        CHECK(error.code == c.code);
        CHECK(error.context == c.context);
        CHECK_FALSE(error.message.empty());
    }

    // Mouse deltas take any finite number; the step limit keeps hostile files small.
    CHECK(parseOk(R"([{"fromFrame": 0, "toFrame": 0, "actions": {"look_delta_x": 5000, "look_delta_y": -1e6}}])")
              .valueAt(0, Action::LookDeltaY) == -1e6f);
    json tooLong = json::array();
    for (std::size_t i = 0; i <= InputScript::kMaxSteps; ++i) {
        tooLong.push_back({{"fromFrame", 0}, {"toFrame", 0}, {"actions", {{"back", 1}}}});
    }
    const auto rejected = InputScript::fromJson(tooLong, "long.json");
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().code == ErrorCode::ValidationFailed);
    // Frames built in code as signed integers are range-checked too.
    const auto signedFrame = InputScript::fromJson(
        json::array({{{"fromFrame", 0}, {"toFrame", std::int64_t{5'000'000'000}}, {"actions", {{"back", 1}}}}}), "x");
    REQUIRE_FALSE(signedFrame.has_value());
    CHECK(signedFrame.error().context == "x:/0/toFrame");
}

TEST_CASE("input/input_script: scripted buttons are held at the press threshold with clean edges",
          "[input][input_script]") {
    const InputScript script = parseOk(R"([
        {"fromFrame": 2, "toFrame": 4, "actions": {"screenshot": 1}},
        {"fromFrame": 6, "toFrame": 7, "actions": {"sprint": 0.4}},
        {"fromFrame": 8, "toFrame": 8, "actions": {"sprint": -0.6, "look_delta_x": 300, "look_x": 1}}
    ])");
    const std::vector<ActionState> states = play(script, 10);

    // Screenshot: pressed once on frame 2, held through frame 4, released on frame 5.
    CHECK_FALSE(states[1].held(Action::Screenshot));
    CHECK(states[2].pressed(Action::Screenshot));
    CHECK(states[2].held(Action::Screenshot));
    CHECK(states[2].axis(Action::Screenshot) == 1.0f);
    CHECK_FALSE(states[3].pressed(Action::Screenshot));
    CHECK(states[4].held(Action::Screenshot));
    CHECK(states[5].released(Action::Screenshot));
    CHECK_FALSE(states[5].held(Action::Screenshot));
    CHECK(states[5].axis(Action::Screenshot) == 0.0f);
    CHECK_FALSE(states[6].released(Action::Screenshot));

    // Below the threshold an action carries its value without being held; |value| counts, as in InputSystem.
    CHECK(states[6].axis(Action::Sprint) == 0.4f);
    CHECK_FALSE(states[6].held(Action::Sprint));
    CHECK(states[8].held(Action::Sprint));
    CHECK(states[8].pressed(Action::Sprint));
    CHECK(states[9].released(Action::Sprint));

    // Look actions are never held, however large the value.
    CHECK(states[8].axis(Action::LookDeltaX) == 300.0f);
    CHECK(states[8].axis(Action::LookX) == 1.0f);
    CHECK_FALSE(states[8].held(Action::LookDeltaX));
    CHECK_FALSE(states[8].held(Action::LookX));
    CHECK_FALSE(states[8].pressed(Action::LookX));
}

TEST_CASE("input/input_script: device input passes through and edges stay consistent at script boundaries",
          "[input][input_script]") {
    using rv::input::Key;
    rv::input::InputSystem system;
    InputScriptPlayer player(
        parseOk(R"([{"fromFrame": 1, "toFrame": 2, "actions": {"back": 1, "move_forward": -1}}])"));
    const auto frameWith = [](std::initializer_list<Key> held, std::initializer_list<Key> tapped = {}) {
        rv::input::RawInputFrame frame;
        for (const Key key : held) {
            frame.keysDown.set(rv::input::toIndex(key));
            frame.keysPressed.set(rv::input::toIndex(key));
        }
        for (const Key key : tapped) {
            frame.keysPressed.set(rv::input::toIndex(key));
            frame.keysReleased.set(rv::input::toIndex(key));
        }
        return frame;
    };
    const auto step = [&](std::uint64_t frame, const rv::input::RawInputFrame& raw) {
        ActionState state = system.resolve(raw);
        player.apply(frame, state, kThreshold);
        return state;
    };

    // Frame 0: W held by the device, nothing scripted.
    const ActionState f0 = step(0, frameWith({Key::W}));
    CHECK(f0.axis(Action::MoveForward) == 1.0f);
    CHECK(f0.pressed(Action::MoveForward));
    // Frames 1-2: the script overrides move_forward (back pedal) and holds Back; W stays physically held.
    const ActionState f1 = step(1, frameWith({Key::W}));
    CHECK(f1.axis(Action::MoveForward) == -1.0f);
    CHECK_FALSE(f1.pressed(Action::MoveForward)); // still held: no new press
    CHECK(f1.pressed(Action::Back));
    const ActionState f2 = step(2, frameWith({Key::W}));
    CHECK_FALSE(f2.pressed(Action::Back));
    CHECK(f2.held(Action::Back));
    // Frame 3: the script ends. Back reports its release although InputSystem never saw it held; W carries on.
    const ActionState f3 = step(3, frameWith({Key::W}));
    CHECK(f3.released(Action::Back));
    CHECK_FALSE(f3.held(Action::Back));
    CHECK(f3.axis(Action::MoveForward) == 1.0f);
    CHECK_FALSE(f3.pressed(Action::MoveForward));
    // Frame 4: Escape tapped within the frame (down and up): still a full click.
    const ActionState f4 = step(4, frameWith({}, {Key::Escape}));
    CHECK(f4.pressed(Action::Back));
    CHECK(f4.released(Action::Back));
    CHECK_FALSE(f4.held(Action::Back));
    CHECK(f4.released(Action::MoveForward)); // W was let go
}

TEST_CASE("input/input_script: the verification scripts in tests/input_scripts are valid", "[input][input_script]") {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(sourceDir() / "tests" / "input_scripts")) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        INFO(rv::core::pathToUtf8(entry.path()));
        const auto script = InputScript::loadFromFile(entry.path());
        if (!script) {
            FAIL(script.error().toString());
        }
        CHECK_FALSE(script->steps().empty());
        ++count;
    }
    CHECK(count >= 4);
}

TEST_CASE("input/input_script: loadFromFile reports the file path", "[input][input_script]") {
    const rv::test::TempDir dir;
    const std::filesystem::path missing = dir.path() / "missing.json";
    const auto notFound = InputScript::loadFromFile(missing);
    REQUIRE_FALSE(notFound.has_value());
    CHECK(notFound.error().code == ErrorCode::NotFound);

    const std::filesystem::path broken = dir.path() / "broken.json";
    REQUIRE(rv::core::writeTextFileAtomic(broken, "[{\"fromFrame\": 0,").has_value());
    const auto malformed = InputScript::loadFromFile(broken);
    REQUIRE_FALSE(malformed.has_value());
    CHECK(malformed.error().code == ErrorCode::ParseError);

    const std::filesystem::path invalid = dir.path() / "invalid.json";
    REQUIRE(rv::core::writeTextFileAtomic(invalid, R"([{"fromFrame": 0, "toFrame": 1, "actions": {"jumpp": 1}}])")
                .has_value());
    const auto unknown = InputScript::loadFromFile(invalid);
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().context == rv::core::pathToUtf8(invalid) + ":/0/actions/jumpp");

    const std::filesystem::path huge = dir.path() / "huge.json";
    REQUIRE(rv::core::writeTextFileAtomic(huge, R"([{"fromFrame": 0, "toFrame": 1e400, "actions": {"back": 1}}])")
                .has_value());
    const auto overflow = InputScript::loadFromFile(huge);
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error().code == ErrorCode::ParseError);
}
