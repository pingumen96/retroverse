// The input path of the quick catalog, GPU-free (I2 step 6): devices -> the action map (the shipped defaults with the
// user's bindings.json over them, as app::loadBindings reads them) -> input::InputSystem -> the --input-script player
// -> ui::NavInput -> UiCommands, applied to the model as the screen's list applies them. spike_ui runs the same path on
// the GPU (ctest gpu.spike_ui.input_script and gpu.spike_ui.user_bindings).

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "support/temp_dir.hpp"
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/ui/in_memory_catalog_source.hpp>
#include <rv/ui/nav_input.hpp>
#include <rv/ui/quick_catalog_model.hpp>
#include <rv/ui/ui_command.hpp>

using rv::input::ActionMap;
using rv::input::GamepadButton;
using rv::input::InputSystem;
using rv::input::Key;
using rv::input::RawInputFrame;
using rv::ui::CatalogEntry;
using rv::ui::InMemoryCatalogSource;
using rv::ui::NavInput;
using rv::ui::QuickCatalogModel;
using rv::ui::UiCommand;
using rv::ui::UiCommandList;

namespace {

/// The step of --fixed-dt 0.016667 in the scripted runs.
constexpr double kFixedDt = 0.016667;

std::filesystem::path sourceDir() {
    return rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR);
}

/// `count` games, the ids in the GameId text form.
std::vector<CatalogEntry> catalog(std::size_t count) {
    std::vector<CatalogEntry> games(count);
    for (std::size_t index = 0; index < count; ++index) {
        games[index].id = "g" + std::string(16 - std::to_string(index + 1).size(), '0') + std::to_string(index + 1);
        games[index].title = "Gioco " + std::to_string(index);
        games[index].platform = index % 2 == 0 ? "Arcade" : "Portatile";
    }
    return games;
}

/// The bindings app::loadBindings gives: assets/config/default_bindings.json, then the user's file over it.
ActionMap bindingsWithUserFile(const std::filesystem::path& userFile) {
    auto map = ActionMap::loadFromFile(sourceDir() / "assets" / "config" / "default_bindings.json");
    REQUIRE(map.has_value());
    const auto document = rv::core::readJsonFile(userFile);
    REQUIRE(document.has_value());
    const auto replaced = map->applyOverrides(*document, rv::core::pathToUtf8(userFile));
    if (!replaced) {
        FAIL(replaced.error().toString());
    }
    return std::move(*map);
}

/// The commands of the list region of QuickCatalogScreen on the model: moves, pages, Home and End.
void applyToList(QuickCatalogModel& model, const UiCommandList& commands) {
    for (const UiCommand command : commands.commands()) {
        switch (command) {
        case UiCommand::Up:
            static_cast<void>(model.moveSelection(-1));
            break;
        case UiCommand::Down:
            static_cast<void>(model.moveSelection(1));
            break;
        case UiCommand::PageUp:
            static_cast<void>(model.pageUp());
            break;
        case UiCommand::PageDown:
            static_cast<void>(model.pageDown());
            break;
        case UiCommand::Home:
            static_cast<void>(model.home());
            break;
        case UiCommand::End:
            static_cast<void>(model.end());
            break;
        default:
            break;
        }
    }
}

/// A pad with `button` pressed in this frame (and held at its end).
RawInputFrame padPress(GamepadButton button) {
    RawInputFrame frame;
    frame.gamepadConnected = true;
    frame.gamepadButtonsDown.set(rv::input::toIndex(button));
    frame.gamepadButtonsPressed.set(rv::input::toIndex(button));
    return frame;
}

RawInputFrame keyPress(Key key) {
    RawInputFrame frame;
    frame.keysDown.set(rv::input::toIndex(key));
    frame.keysPressed.set(rv::input::toIndex(key));
    return frame;
}

/// The commands a device frame gives through `input` and `nav`, then a frame with everything released.
std::vector<UiCommand> commandsOf(InputSystem& input, NavInput& nav, const RawInputFrame& frame) {
    UiCommandList list;
    nav.update(input.resolve(frame), kFixedDt, list);
    std::vector<UiCommand> commands(list.commands().begin(), list.commands().end());
    UiCommandList released;
    RawInputFrame idle;
    idle.gamepadConnected = frame.gamepadConnected;
    nav.update(input.resolve(idle), kFixedDt, released);
    return commands;
}

} // namespace

TEST_CASE("ui/quick_catalog_input: a user's bindings.json reaches the UI", "[ui][quick_catalog_input]") {
    rv::test::TempDir dir("rv-ui-bindings");
    const std::filesystem::path userFile = dir.file("bindings.json");
    // ui_down on the pad's X and the J key alone; ui_info unbound. The other actions keep their defaults.
    REQUIRE(rv::core::writeTextFileAtomic(userFile, R"({
        "schemaVersion": 1,
        "bindings": {
            "ui_down": [{"padButton": "west"}, {"key": "j"}],
            "ui_info": []
        }
    })"));
    const ActionMap bindings = bindingsWithUserFile(userFile);
    InputSystem input(bindings);
    NavInput nav;
    using Commands = std::vector<UiCommand>;

    CHECK(commandsOf(input, nav, padPress(GamepadButton::West)) == Commands{UiCommand::Down}); // was Informazioni
    CHECK(commandsOf(input, nav, keyPress(Key::J)) == Commands{UiCommand::Down});
    CHECK(commandsOf(input, nav, padPress(GamepadButton::DpadDown)).empty()); // no longer bound
    CHECK(commandsOf(input, nav, keyPress(Key::Down)).empty());
    CHECK(commandsOf(input, nav, padPress(GamepadButton::DpadUp)) == Commands{UiCommand::Up}); // a default kept
    CHECK(commandsOf(input, nav, padPress(GamepadButton::South)) == Commands{UiCommand::Activate});
    // The keys the screen keeps away from RmlUi follow the bindings: J is a command now, the arrow down is a key again.
    const auto keys = rv::ui::commandKeys(bindings);
    CHECK(keys.test(rv::input::toIndex(Key::J)));
    CHECK_FALSE(keys.test(rv::input::toIndex(Key::Down)));

    // On the model: X moves the selection down, the D-pad down does not.
    InMemoryCatalogSource source(catalog(100));
    QuickCatalogModel model(source, 7);
    for (const RawInputFrame& frame :
         {padPress(GamepadButton::DpadDown), padPress(GamepadButton::West), padPress(GamepadButton::West)}) {
        UiCommandList list;
        nav.update(input.resolve(frame), kFixedDt, list);
        applyToList(model, list);
        UiCommandList released;
        nav.update(input.resolve(RawInputFrame{}), kFixedDt, released);
    }
    CHECK(model.selectedPosition() == std::optional<std::size_t>(2));
}

TEST_CASE("ui/quick_catalog_input: tests/input_scripts/quick_catalog_select.json moves the selection",
          "[ui][quick_catalog_input]") {
    auto script =
        rv::input::InputScript::loadFromFile(sourceDir() / "tests" / "input_scripts" / "quick_catalog_select.json");
    if (!script) {
        FAIL(script.error().toString());
    }
    // Played as spike_ui plays it (--fixed-dt 0.016667, frames counted from 0) over idle devices and 100,000 games.
    InMemoryCatalogSource source(catalog(100000));
    QuickCatalogModel model(source, 7);
    InputSystem input;
    rv::input::InputScriptPlayer player(std::move(*script));
    NavInput nav;
    std::vector<std::size_t> positions;
    for (std::uint64_t frame = 0; frame < 120; ++frame) {
        rv::input::ActionState actions = input.resolve(RawInputFrame{});
        player.apply(frame, actions, input.settings().pressThreshold);
        UiCommandList commands;
        nav.update(actions, kFixedDt, commands);
        applyToList(model, commands);
        if (frame == 15 || frame == 85 || frame == 95) {
            positions.push_back(model.selectedPosition().value_or(0));
        }
    }
    // A tap down, the stick held for 1 s (7 steps), End, a tap up.
    CHECK(positions == std::vector<std::size_t>{1, 8, 99999});
    CHECK(model.selectedPosition() == std::optional<std::size_t>(99998));
    CHECK(model.windowFirst() == 100000 - 7);
}
