// The hints from the active bindings (ADR-0018 §7): what a hint-action shows on each device; every document's hints name
// real actions, come from the map and link the hints' style sheet.

#include <rv/ui/hint_bar.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/input/binding_labels.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <regex>
#include <sstream>
#include <string>

using rv::input::Action;
using rv::input::ActionMap;
using rv::input::DeviceKind;
using rv::ui::HintBar;
using rv::ui::HintGlyph;

namespace {

HintBar shippedBar() {
    HintBar bar;
    auto labels = rv::input::KeyLabels::loadFromFile(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR "/ui/key_labels.json"));
    REQUIRE(labels.has_value());
    bar.setLabels(std::move(*labels));
    bar.setMap(ActionMap::defaults());
    return bar;
}

} // namespace

TEST_CASE("ui/hint_bar: a hint follows the device used last", "[ui][hints]") {
    HintBar bar = shippedBar();
    CHECK(bar.glyph("ui_activate") == HintGlyph{"Invio", "", true, true});
    bar.setDevice(DeviceKind::Gamepad);
    CHECK(bar.glyph("ui_activate") == HintGlyph{"A", "pad-a", false, true});
    CHECK(bar.glyph("ui_back") == HintGlyph{"B", "pad-b", false, true});
    CHECK(bar.glyph("ui_info") == HintGlyph{"X", "pad-x", false, true});
    CHECK(bar.glyph("ui_media") == HintGlyph{"Y", "pad-y", false, true});
}

TEST_CASE("ui/hint_bar: a hint follows the map", "[ui][hints]") {
    HintBar bar = shippedBar();
    ActionMap map = ActionMap::defaults();
    map.clear(Action::UiActivate);
    map.bind(Action::UiActivate, rv::input::KeyBinding{rv::input::Key::Insert, 1.0f});
    map.bind(Action::UiActivate, rv::input::GamepadButtonBinding{rv::input::GamepadButton::West, 1.0f});
    bar.setMap(map);
    CHECK(bar.glyph("ui_activate").text == "Ins");
    bar.setDevice(DeviceKind::Gamepad);
    CHECK(bar.glyph("ui_activate") == HintGlyph{"X", "pad-x", false, true}); // the colour goes with the button
}

TEST_CASE("ui/hint_bar: several actions name their buttons, or a group's one name", "[ui][hints]") {
    HintBar bar = shippedBar();
    CHECK(bar.glyph("ui_filter_previous ui_filter_next") == HintGlyph{"F2 F3", "", true, true});
    CHECK(bar.glyph("ui_up ui_down ui_left ui_right").text == "Frecce");
    CHECK(bar.glyph("ui_up ui_down").text == "Frecce");
    bar.setDevice(DeviceKind::Gamepad);
    CHECK(bar.glyph("ui_filter_previous ui_filter_next") == HintGlyph{"LB RB", "", true, true});
    CHECK(bar.glyph("ui_up ui_down ui_left ui_right").text == "Croce");
    CHECK(bar.glyph("ui_up ui_down").text == "^ v");
    CHECK(bar.glyph("ui_left ui_right").text == "< >");
}

TEST_CASE("ui/hint_bar: a hint with nothing bound says so, and so does a name that is no action", "[ui][hints]") {
    HintBar bar = shippedBar();
    bar.setDevice(DeviceKind::Gamepad);
    CHECK_FALSE(bar.glyph("toggle_mute").bound);
    CHECK_FALSE(bar.glyph("no_such_action").bound);
    CHECK_FALSE(bar.glyph("").bound);
    // One of two bound: that one shows.
    CHECK(bar.glyph("toggle_mute ui_back") == HintGlyph{"B", "pad-b", false, true});
}

TEST_CASE("ui/hint_bar: a prompt reads even when the device has nothing bound", "[ui][hints]") {
    HintBar bar = shippedBar();
    ActionMap map;
    map.bind(Action::Interact, rv::input::GamepadButtonBinding{rv::input::GamepadButton::South, 1.0f});
    bar.setMap(map);
    CHECK(bar.label(Action::Interact, DeviceKind::KeyboardMouse).empty());
    CHECK(bar.promptLabel(Action::Interact, DeviceKind::KeyboardMouse) == "A");
    map = ActionMap{};
    map.bind(Action::Interact, rv::input::KeyBinding{rv::input::Key::E, 1.0f});
    bar.setMap(map);
    CHECK(bar.promptLabel(Action::Interact, DeviceKind::Gamepad) == "E");
    CHECK(bar.promptLabel(Action::Interact, DeviceKind::KeyboardMouse) == "E");
    // Nothing anywhere: no name, and the document drops the glyph.
    bar.setMap(ActionMap{});
    CHECK(bar.promptLabel(Action::Interact, DeviceKind::Gamepad).empty());
}

TEST_CASE("ui/hint_bar: the revision changes with what the glyphs say, not otherwise", "[ui][hints]") {
    HintBar bar = shippedBar();
    const std::uint64_t before = bar.revision();
    bar.setDevice(DeviceKind::KeyboardMouse);
    bar.setMap(ActionMap::defaults());
    CHECK(bar.revision() == before);
    bar.setDevice(DeviceKind::Gamepad);
    CHECK(bar.revision() == before + 1);
}

TEST_CASE("ui/hint_bar: every document's hints come from the map", "[ui][hints][documents]") {
    const std::filesystem::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR "/ui");
    const std::regex attribute("hint-action=\"([^\"]*)\"");
    // A glyph written by hand: a pad's or a key's letter between the tags of a hint's span.
    const std::regex written("class=\"(pad|kbd|key) pad-[abxy]");
    int hints = 0;
    for (const auto& entry : std::filesystem::directory_iterator(assets)) {
        if (entry.path().extension() != ".rml") {
            continue;
        }
        INFO(rv::core::pathToUtf8(entry.path().filename()));
        auto text = rv::core::readTextFile(entry.path());
        REQUIRE(text.has_value());
        CHECK_FALSE(std::regex_search(*text, written));
        bool declares = false;
        for (std::sregex_iterator at(text->begin(), text->end(), attribute), end; at != end; ++at) {
            declares = true;
            ++hints;
            std::istringstream names((*at)[1].str());
            std::string name;
            bool any = false;
            while (names >> name) {
                INFO(name);
                any = true;
                CHECK(rv::input::actionFromName(name).has_value());
            }
            CHECK(any);
        }
        if (declares) {
            CHECK(text->find("href=\"hint_bar.rcss\"") != std::string::npos);
        }
    }
    CHECK(hints > 30);
}
