// Remapping's rules (ADR-0018 §7): what the Controlli group lists, clashes judged per context, the actions that may never be
// left unbound, the swap, settings.json over a hand-written file, a capture's raw press; and the pad's notice.

#include <rv/app/binding_rules.hpp>
#include <rv/app/controller_watch.hpp>
#include <rv/app/setting_rows.hpp>
#include <rv/app/settings_store.hpp>

#include <rv/core/settings_file.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/binding_labels.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "support/temp_dir.hpp"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

using namespace rv::app;
using namespace rv::input;
using Json = nlohmann::json;

namespace {

Binding key(Key code) {
    return KeyBinding{code, 1.0f};
}

Binding button(GamepadButton code) {
    return GamepadButtonBinding{code, 1.0f};
}

bool holds(const ActionMap& map, Action action, const Binding& press) {
    const auto bindings = map.bindings(action);
    return std::any_of(bindings.begin(), bindings.end(),
                       [&](const Binding& binding) { return sameInput(binding, press); });
}

} // namespace

TEST_CASE("app/binding_rules: the list holds the buttons a user may bind, the locked actions in it", "[app][binding]") {
    CHECK(isBindable(Action::UiActivate));
    CHECK(isBindable(Action::ToggleMute)); // ADR-0018 §7: it can take a pad button here
    CHECK_FALSE(isBindable(Action::MoveForward));
    CHECK_FALSE(isBindable(Action::UiUp)); // the arrows always walk the screen
    CHECK_FALSE(isBindable(Action::ToggleDevUI));
    for (const Action action : {Action::UiActivate, Action::UiBack, Action::Menu}) {
        CHECK(isLockedAction(action));
        CHECK(isBindable(action));
    }
    CHECK_FALSE(isLockedAction(Action::Interact));
}

TEST_CASE("app/binding_rules: the shipped map keeps a key and a pad button for the locked actions", "[app][binding]") {
    const ActionMap defaults = ActionMap::defaults();
    for (const Action action : {Action::UiActivate, Action::UiBack, Action::Menu}) {
        INFO(actionName(action));
        CHECK(primaryBinding(defaults, action, DeviceKind::KeyboardMouse).has_value());
        CHECK(primaryBinding(defaults, action, DeviceKind::Gamepad).has_value());
    }
    // Step 1's gap: the group and Ripristina have their keys.
    for (const Action action : {Action::UiFilterPrevious, Action::UiFilterNext, Action::UiInfo, Action::UiMedia}) {
        INFO(actionName(action));
        CHECK(primaryBinding(defaults, action, DeviceKind::KeyboardMouse).has_value());
    }
}

TEST_CASE("app/binding_rules: a clash is judged per context", "[app][binding]") {
    const ActionMap map = ActionMap::defaults();
    // Free.
    CHECK(judgeBinding(map, Action::UiActivate, key(Key::Insert)).verdict == BindingVerdict::Free);
    // Already its own.
    CHECK(judgeBinding(map, Action::UiActivate, key(Key::Enter)).verdict == BindingVerdict::Same);
    // In the UI's context two actions on one button clash; the world's Back on East does not count there.
    const BindingJudgement ui = judgeBinding(map, Action::UiActivate, button(GamepadButton::East));
    CHECK(ui.verdict == BindingVerdict::Clash);
    CHECK(ui.other == Action::UiBack);
    // World and UI share buttons on purpose: Interact may take the west button UiInfo has (edit_pin clashes, in the world).
    const BindingJudgement world = judgeBinding(map, Action::Interact, button(GamepadButton::West));
    CHECK(world.verdict == BindingVerdict::Clash);
    CHECK(world.other == Action::EditPin);
    CHECK(judgeBinding(map, Action::Sprint, button(GamepadButton::DpadUp)).verdict == BindingVerdict::Free);
    // UiMedia would hand Y to Map, which QuickCatalog has where Map is read: refused, not moved.
    const BindingJudgement moved = judgeBinding(map, Action::UiMedia, button(GamepadButton::Back));
    CHECK(moved.verdict == BindingVerdict::Reserved);
    CHECK(moved.other == Action::QuickCatalog);
    // The shipped sharings are no clash.
    CHECK(judgeBinding(map, Action::Menu, button(GamepadButton::Start)).verdict == BindingVerdict::Same);
    CHECK_FALSE(actionsCompete(Action::QuickCatalog, Action::UiMedia));
    CHECK_FALSE(actionsCompete(Action::Confirm, Action::Menu));
    CHECK(actionsCompete(Action::UiActivate, Action::UiBack));
    CHECK_FALSE(actionsCompete(Action::Interact, Action::UiActivate));
    CHECK(actionsCompete(Action::ToggleMute, Action::UiActivate)); // read everywhere
}

TEST_CASE("app/binding_rules: what stays what it is", "[app][binding]") {
    const ActionMap map = ActionMap::defaults();
    // Actions not on the list: the arrows, WASD, the developer's keys.
    const BindingJudgement arrow = judgeBinding(map, Action::UiActivate, button(GamepadButton::DpadUp));
    CHECK(arrow.verdict == BindingVerdict::Reserved);
    CHECK(arrow.other == Action::UiUp);
    CHECK(judgeBinding(map, Action::Sprint, key(Key::W)).verdict == BindingVerdict::Reserved);
    CHECK(judgeBinding(map, Action::ToggleMute, key(Key::F1)).verdict == BindingVerdict::Reserved);
    // A text field's keys never go to a UI action; the world may take them.
    CHECK(judgeBinding(map, Action::UiActivate, key(Key::K)).verdict == BindingVerdict::Reserved);
    CHECK(judgeBinding(map, Action::UiMedia, key(Key::Space)).verdict == BindingVerdict::Reserved);
    CHECK(judgeBinding(map, Action::Sprint, key(Key::K)).verdict == BindingVerdict::Free);
}

TEST_CASE("app/binding_rules: the on-screen keyboard's Confirm is judged in the UI, Enter's sharing aside",
          "[app][binding]") {
    const ActionMap map = ActionMap::defaults();
    // Read in the UI since I7 step 5: Start for Informazioni, which deletes on the keyboard, would do two things there.
    CHECK(actionsCompete(Action::Confirm, Action::UiInfo));
    CHECK_FALSE(actionsCompete(Action::Confirm, Action::UiActivate)); // Enter, shipped so
    CHECK_FALSE(actionsCompete(Action::Confirm, Action::Menu));       // Start, shipped so
    CHECK(judgeBinding(map, Action::UiInfo, button(GamepadButton::Start)).verdict != BindingVerdict::Free);
    // A letter types in the field the keyboard reads Confirm over.
    CHECK(judgeBinding(map, Action::Confirm, key(Key::K)).verdict == BindingVerdict::Reserved);
    CHECK(judgeBinding(map, Action::Confirm, key(Key::Insert)).verdict == BindingVerdict::Free);
    // Enter, shared with it, costs UiActivate no swap of its key.
    CHECK(judgeBinding(map, Action::UiActivate, key(Key::Escape)).verdict == BindingVerdict::Clash);
    // A holder the world reads gets no such pass: Enter for QuickCatalog would open the catalog and be Confirm in free roam.
    const BindingJudgement tab = judgeBinding(map, Action::UiActivate, key(Key::Tab));
    CHECK(tab.verdict == BindingVerdict::Reserved);
    CHECK(tab.other == Action::Confirm);
}

TEST_CASE("app/binding_rules: a locked action never loses its only key or button", "[app][binding]") {
    ActionMap map = ActionMap::defaults();
    map.clear(Action::UiInfo); // no slot to give back
    // UiInfo has nothing to swap: UiActivate would be left without its pad button.
    const BindingJudgement locked = judgeBinding(map, Action::UiInfo, button(GamepadButton::South));
    CHECK(locked.verdict == BindingVerdict::Locked);
    CHECK(locked.other == Action::UiActivate);
    // With a slot the swap keeps it bound.
    const ActionMap shipped = ActionMap::defaults();
    CHECK(judgeBinding(shipped, Action::UiInfo, button(GamepadButton::South)).verdict == BindingVerdict::Clash);
    const ActionMap swapped = withSwap(shipped, Action::UiInfo, button(GamepadButton::South));
    CHECK(holds(swapped, Action::UiInfo, button(GamepadButton::South)));
    CHECK(holds(swapped, Action::UiActivate, button(GamepadButton::West)));
    CHECK_FALSE(holds(swapped, Action::UiActivate, button(GamepadButton::South)));
    CHECK(holds(swapped, Action::UiActivate, key(Key::Enter))); // the other device untouched
}

TEST_CASE("app/binding_rules: with nothing to give back no action is stripped of its device", "[app][binding]") {
    const ActionMap map = ActionMap::defaults();
    // ToggleMute has no pad button: A would leave a pad user without Interact and without UiActivate.
    const BindingJudgement mute = judgeBinding(map, Action::ToggleMute, button(GamepadButton::South));
    CHECK(mute.verdict == BindingVerdict::Locked);
    CHECK(mute.other == Action::Interact);
    // A holder with another button of that device may lose this one.
    ActionMap second = map;
    second.bind(Action::Interact, button(GamepadButton::DpadUp));
    second.clear(Action::UiActivate);
    second.bind(Action::UiActivate, key(Key::Enter));
    second.bind(Action::UiActivate, button(GamepadButton::RightStick));
    const BindingJudgement judged = judgeBinding(second, Action::ToggleMute, button(GamepadButton::South));
    CHECK(judged.verdict == BindingVerdict::Clash);
    const ActionMap taken = withSwap(second, Action::ToggleMute, button(GamepadButton::South));
    CHECK(holds(taken, Action::ToggleMute, button(GamepadButton::South)));
    CHECK_FALSE(holds(taken, Action::Interact, button(GamepadButton::South)));
    CHECK(holds(taken, Action::Interact, button(GamepadButton::DpadUp)));
}

TEST_CASE("app/binding_rules: a swap takes the press from every holder in the context", "[app][binding]") {
    // ToggleMute is read everywhere: X is EditPin's in the world and UiInfo's in the UI.
    ActionMap map = ActionMap::defaults();
    map.bind(Action::ToggleMute, button(GamepadButton::Guide));
    const std::vector<Action> holders = bindingHolders(map, Action::ToggleMute, button(GamepadButton::West));
    CHECK(holders == std::vector<Action>{Action::UiInfo, Action::EditPin});
    REQUIRE(judgeBinding(map, Action::ToggleMute, button(GamepadButton::West)).verdict == BindingVerdict::Clash);
    const ActionMap swapped = withSwap(map, Action::ToggleMute, button(GamepadButton::West));
    CHECK(holds(swapped, Action::ToggleMute, button(GamepadButton::West)));
    for (const Action holder : holders) {
        INFO(actionName(holder));
        CHECK_FALSE(holds(swapped, holder, button(GamepadButton::West)));
        CHECK(holds(swapped, holder, button(GamepadButton::Guide)));
    }
}

TEST_CASE("app/binding_rules: a press takes its device's slot alone", "[app][binding]") {
    const ActionMap map = ActionMap::defaults();
    const ActionMap edited = withBinding(map, Action::Interact, key(Key::F));
    CHECK(holds(edited, Action::Interact, key(Key::F)));
    CHECK_FALSE(holds(edited, Action::Interact, key(Key::E)));
    CHECK(holds(edited, Action::Interact, Binding{MouseButtonBinding{MouseButton::Left, 1.0f}})); // the click stays
    CHECK(holds(edited, Action::Interact, button(GamepadButton::South)));
    // The slot's place: the hints go on naming it first.
    CHECK(bindingCode(*primaryBinding(edited, Action::Interact, DeviceKind::KeyboardMouse)) == "f");
    // A trigger counts as a pad's press.
    const Binding trigger = GamepadAxisBinding{GamepadAxis::LeftTrigger};
    const ActionMap pulled = withBinding(map, Action::UiActivate, trigger);
    CHECK(holds(pulled, Action::UiActivate, trigger));
    CHECK_FALSE(holds(pulled, Action::UiActivate, button(GamepadButton::South)));
    // An action with no slot gets one.
    CHECK(holds(withBinding(map, Action::ToggleMute, button(GamepadButton::Back)), Action::ToggleMute,
                button(GamepadButton::Back)));
    // A list as a binding file holds it.
    const Json list = bindingList(edited, Action::Interact);
    CHECK(list.is_array());
    CHECK(list.size() == 3);
    CHECK(list[0]["key"] == "f");
}

TEST_CASE("app/binding_rules: whatever a file says, the locked actions keep theirs", "[app][binding]") {
    const ActionMap shipped = ActionMap::defaults();
    ActionMap map = shipped;
    map.clear(Action::UiActivate);
    map.clear(Action::Menu);
    map.bind(Action::Menu, button(GamepadButton::Back));
    const std::vector<Action> kept = keepUnlocked(map, shipped);
    CHECK(std::count(kept.begin(), kept.end(), Action::UiActivate) == 2);
    CHECK(std::count(kept.begin(), kept.end(), Action::Menu) == 1);
    CHECK(holds(map, Action::UiActivate, key(Key::Enter)));
    CHECK(holds(map, Action::UiActivate, button(GamepadButton::South)));
    CHECK(holds(map, Action::Menu, key(Key::F10)));
    CHECK(holds(map, Action::Menu, button(GamepadButton::Back))); // the file's button stays
    CHECK(keepUnlocked(map, shipped).empty());
}

TEST_CASE("app/binding_rules: settings.json over a hand-written file names only what the screen changed",
          "[app][binding]") {
    // The user's bindings.json moved Interact and unbound Sprint's key.
    ActionMap map = ActionMap::defaults();
    const Json written = Json::parse(R"({"schemaVersion": 1, "bindings": {
        "interact": [{"key": "f"}], "sprint": [{"padButton": "left_stick"}]}})");
    REQUIRE(map.applyOverrides(written, "bindings.json").has_value());

    const rv::test::TempDir dir("binding_rules");
    rv::core::SettingsFile file = rv::core::SettingsFile::load(dir.path() / "settings.json");
    file.set(kBindingsSettingsSection, "ui_info", Json::parse(R"([{"key": "f7"}])"));
    CHECK(applySettingsBindings(map, file) == 1);

    CHECK(holds(map, Action::UiInfo, key(Key::F7)));
    CHECK(holds(map, Action::Interact, key(Key::F))); // the hand-written ones stay
    CHECK(map.bindings(Action::Sprint).size() == 1);
}

TEST_CASE("app/binding_rules: a bad entry of the section is left out alone, on every path", "[app][binding]") {
    const Json section = Json::parse(R"({"sprint": [{"key": "bogus"}], "defaults": true,
        "ui_info": [{"key": "f7"}], "ui_media": "not a list"})");
    ActionMap live = ActionMap::defaults();
    CHECK(applyBindingSection(live, section) == 1);
    CHECK(holds(live, Action::UiInfo, key(Key::F7)));
    CHECK(holds(live, Action::Sprint, key(Key::LeftShift)));
    CHECK(holds(live, Action::UiMedia, key(Key::F6)));

    const rv::test::TempDir dir("binding_section");
    rv::core::SettingsFile file = rv::core::SettingsFile::load(dir.path() / "settings.json");
    file.set(kBindingsSettingsSection, "sprint", section["sprint"]);
    file.set(kBindingsSettingsSection, "ui_info", section["ui_info"]);
    ActionMap loaded = ActionMap::defaults();
    CHECK(applySettingsBindings(loaded, file) == 1);
    CHECK(loaded == live);
}

TEST_CASE("app/binding_rules: a row's list equal to the layer below leaves the file", "[app][binding]") {
    const rv::test::TempDir dir("binding_rows");
    SettingsLayers layers;
    const ActionMap below = ActionMap::defaults();
    layers.bindings = below.toJson()["bindings"];
    SettingsStore store(rv::core::SettingsFile::load(dir.path() / "settings.json"), layers, settingRows({}));
    const SettingRow* row = store.row("bindings.ui_activate");
    REQUIRE(row != nullptr);
    CHECK(row->kind == SettingKind::Binding);
    CHECK_FALSE(store.overridden(*row));
    REQUIRE(store.set(*row, bindingList(withBinding(below, Action::UiActivate, key(Key::F7)), Action::UiActivate)));
    CHECK(store.overridden(*row));
    CHECK(store.value(*row)[0]["key"] == "f7");
    // Bound back as it was: no key.
    REQUIRE(store.set(*row, bindingList(below, Action::UiActivate)));
    CHECK_FALSE(store.overridden(*row));
    // The list ends with the button that clears the section, which holds no value.
    const SettingRow* restore = store.row("bindings.defaults");
    REQUIRE(restore != nullptr);
    CHECK(restore->kind == SettingKind::Action);
    CHECK_FALSE(store.set(*restore, Json::array()));
}

TEST_CASE("app/binding_rules: a capture reads edges, keys first, never what was down when it started",
          "[app][binding]") {
    RawInputFrame frame;
    frame.gamepadConnected = true;
    frame.keysDown.set(toIndex(Key::Enter)); // the press that started it, still held
    frame.gamepadAxes[toIndex(GamepadAxis::RightTrigger)] = 1.0f;
    CapturePress capture;
    capture.start(frame);
    CHECK_FALSE(capture.next(frame).has_value());

    RawInputFrame west = frame;
    west.gamepadButtonsPressed.set(toIndex(GamepadButton::West));
    const auto pad = capture.next(west);
    REQUIRE(pad);
    CHECK(sameInput(*pad, button(GamepadButton::West)));

    RawInputFrame both = frame;
    both.keysPressed.set(toIndex(Key::F7));
    both.gamepadButtonsPressed.set(toIndex(GamepadButton::West));
    REQUIRE(capture.next(both));
    CHECK(sameInput(*capture.next(both), key(Key::F7)));

    // A trigger counts when it crosses half-way, after being let go.
    RawInputFrame released = frame;
    released.gamepadAxes[toIndex(GamepadAxis::RightTrigger)] = 0.0f;
    CHECK_FALSE(capture.next(released).has_value());
    const auto trigger = capture.next(frame);
    REQUIRE(trigger);
    CHECK(sameInput(*trigger, Binding{GamepadAxisBinding{GamepadAxis::RightTrigger}}));
    CHECK_FALSE(capture.next(frame).has_value());

    // A mouse button is never captured.
    RawInputFrame click;
    click.mouseButtonsPressed.set(toIndex(MouseButton::Left));
    CHECK_FALSE(capture.next(click).has_value());
    // No pad: its state counts for nothing.
    RawInputFrame gone = west;
    gone.gamepadConnected = false;
    CHECK_FALSE(capture.next(gone).has_value());
}

TEST_CASE("app/controller_watch: the notice shows when the last pad goes, until one comes back or time passes",
          "[app][binding]") {
    ControllerWatch watch;
    watch.update(false, 0.016);
    CHECK_FALSE(watch.noticeShown()); // none at the start: nothing went
    watch.update(true, 0.016);
    CHECK_FALSE(watch.noticeShown());
    CHECK(watch.back() == 1);

    watch.update(false, 0.016);
    CHECK(watch.noticeShown());
    CHECK(watch.lost() == 1);
    watch.update(false, ControllerWatch::kNoticeSeconds / 2.0);
    CHECK(watch.noticeShown());
    watch.update(true, 0.016);
    CHECK_FALSE(watch.noticeShown());
    CHECK(watch.back() == 2);

    watch.update(false, 0.016);
    watch.update(false, ControllerWatch::kNoticeSeconds + 1.0);
    CHECK_FALSE(watch.noticeShown());
    CHECK(watch.lost() == 2);
}
