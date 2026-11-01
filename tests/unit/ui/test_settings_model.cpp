// The settings screen's model (ADR-0018 §3): rows of a group at a time, every value by the pad's commands, clicks
// mirroring the keys.

#include <rv/ui/settings_model.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

using rv::ui::SettingsClick;
using rv::ui::SettingsCommand;
using rv::ui::SettingsModel;
using rv::ui::SettingsRow;
using rv::ui::SettingsRowKind;
using rv::ui::UiCommand;
using Json = nlohmann::json;

namespace {

SettingsRow toggle(std::string key, std::string group, bool on) {
    SettingsRow row;
    row.key = std::move(key);
    row.group = std::move(group);
    row.kind = SettingsRowKind::Toggle;
    row.value = on;
    return row;
}

SettingsRow choice(std::string key, std::string group, Json value, std::vector<Json> choices) {
    SettingsRow row;
    row.key = std::move(key);
    row.group = std::move(group);
    row.kind = SettingsRowKind::Choice;
    row.value = std::move(value);
    row.choices = std::move(choices);
    return row;
}

SettingsRow slider(std::string key, std::string group, Json value, double min, double max, double step,
                   bool nullable = false) {
    SettingsRow row;
    row.key = std::move(key);
    row.group = std::move(group);
    row.kind = SettingsRowKind::Slider;
    row.value = std::move(value);
    row.min = min;
    row.max = max;
    row.step = step;
    row.nullable = nullable;
    return row;
}

std::vector<SettingsRow> table() {
    return {choice("mode", "display", "window", {"window", "borderless"}),
            slider("fov", "display", nullptr, 70.0, 110.0, 5.0, true),
            slider("master", "audio", 1.0, 0.0, 1.0, 0.05),
            toggle("mute", "audio", false),
            toggle("vr-only", "vr", false)};
}

SettingsCommand change(std::string key, Json value) {
    return SettingsCommand{SettingsCommand::Kind::Change, std::move(key), std::move(value)};
}

} // namespace

TEST_CASE("ui/settings: opens on the first group with rows; the bumpers walk the groups that hold some", "[ui][settings]") {
    SettingsModel model;
    model.setRows(table());
    model.open();
    CHECK(model.group() == "display");
    CHECK(model.groups() == std::vector<std::string>{"display", "audio", "vr"});
    REQUIRE(model.shown().size() == 2);
    CHECK(model.focusedElement() == "st-row-0");

    static_cast<void>(model.apply(UiCommand::FilterNext));
    CHECK(model.group() == "audio");
    static_cast<void>(model.apply(UiCommand::FilterNext));
    CHECK(model.group() == "vr");
    static_cast<void>(model.apply(UiCommand::FilterNext));
    CHECK(model.group() == "display");
    static_cast<void>(model.apply(UiCommand::FilterPrevious));
    CHECK(model.group() == "vr");
}

TEST_CASE("ui/settings: the rows, then Ripristina tutto; the focus stops at the ends", "[ui][settings]") {
    SettingsModel model;
    model.setRows(table());
    model.open();
    static_cast<void>(model.apply(UiCommand::Up));
    CHECK(model.focus() == 0);
    static_cast<void>(model.apply(UiCommand::Down));
    static_cast<void>(model.apply(UiCommand::Down));
    CHECK(model.focusedElement() == std::string(rv::ui::settings::kRestoreAllId));
    static_cast<void>(model.apply(UiCommand::Down));
    CHECK(model.focus() == 2);
}

TEST_CASE("ui/settings: a toggle flips, on Left, Right and Activate", "[ui][settings]") {
    SettingsModel model;
    model.setRows(table());
    model.open();
    static_cast<void>(model.apply(UiCommand::FilterNext));
    static_cast<void>(model.apply(UiCommand::Down));
    CHECK(model.apply(UiCommand::Activate) == change("mute", true));
    CHECK(model.apply(UiCommand::Left) == change("mute", true));
    CHECK(model.apply(UiCommand::Right) == change("mute", true));
}

TEST_CASE("ui/settings: a choice moves along; Activate comes round, the arrows stop", "[ui][settings]") {
    SettingsModel model;
    model.setRows(table());
    model.open();
    CHECK(model.apply(UiCommand::Right) == change("mode", "borderless"));
    CHECK_FALSE(model.apply(UiCommand::Left).has_value());
    CHECK(model.apply(UiCommand::Activate) == change("mode", "borderless"));

    std::vector<SettingsRow> rows = table();
    rows[0].value = "borderless";
    model.setRows(rows);
    CHECK_FALSE(model.apply(UiCommand::Right).has_value());
    CHECK(model.apply(UiCommand::Left) == change("mode", "window"));
    CHECK(model.apply(UiCommand::Activate) == change("mode", "window"));
}

TEST_CASE("ui/settings: a choice holding a value it does not list goes to its first", "[ui][settings]") {
    const SettingsRow row = choice("list", "world", "explicit", {"shuffle", "recent"});
    CHECK(rv::ui::settingsStep(row, 1, false) == Json("shuffle"));
    CHECK(rv::ui::settingsValueKey(row) == "explicit");
}

TEST_CASE("ui/settings: a slider steps, snapped to its steps, within its range", "[ui][settings]") {
    SettingsRow master = slider("master", "audio", 0.33, 0.0, 1.0, 0.05);
    CHECK(rv::ui::settingsStep(master, 1, false) == Json(0.4)); // from a hand-written value: its nearest step's next
    CHECK(rv::ui::settingsStep(master, -1, false) == Json(0.3));
    master.value = 1.0;
    CHECK_FALSE(rv::ui::settingsStep(master, 1, false).has_value());
    CHECK(rv::ui::settingsStep(master, -1, false) == Json(0.95));
    master.value = 0.0;
    CHECK_FALSE(rv::ui::settingsStep(master, -1, false).has_value());
    // Twenty steps add up without binary noise.
    master.value = 0.3;
    CHECK(rv::ui::settingsStep(master, 1, false) == Json(0.35));
    CHECK(rv::ui::settingsValueText(master) == "30");
    CHECK(rv::ui::settingsFill(master) == "30%");
}

TEST_CASE("ui/settings: the FOV's scene lies below its range", "[ui][settings]") {
    SettingsRow fov = slider("fov", "display", nullptr, 70.0, 110.0, 5.0, true);
    CHECK(rv::ui::settingsStep(fov, 1, false) == Json(70.0));
    CHECK_FALSE(rv::ui::settingsStep(fov, -1, false).has_value());
    CHECK(rv::ui::settingsValueText(fov).empty());
    fov.value = 70.0;
    CHECK(rv::ui::settingsStep(fov, -1, false) == Json(nullptr));
    CHECK(rv::ui::settingsStep(fov, 1, false) == Json(75.0));
    CHECK(rv::ui::settingsValueText(fov) == "70");
    fov.value = 1.25;
    CHECK(rv::ui::settingsValueKey(fov) == "1.25");
}

TEST_CASE("ui/settings: X restores a row settings.json holds", "[ui][settings]") {
    SettingsModel model;
    std::vector<SettingsRow> rows = table();
    model.setRows(rows);
    model.open();
    CHECK_FALSE(model.apply(UiCommand::Info).has_value());
    rows[0].overridden = true;
    model.setRows(rows);
    const auto command = model.apply(UiCommand::Info);
    REQUIRE(command.has_value());
    CHECK(command->kind == SettingsCommand::Kind::Restore);
    CHECK(command->key == "mode");
}

TEST_CASE("ui/settings: Ripristina tutto asks once; Back and moving away drop the question", "[ui][settings]") {
    SettingsModel model;
    model.setRows(table());
    model.open();
    static_cast<void>(model.apply(UiCommand::Down));
    static_cast<void>(model.apply(UiCommand::Down));
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    CHECK(model.confirmingRestoreAll());
    CHECK_FALSE(model.apply(UiCommand::Back).has_value());
    CHECK(model.isOpen());
    CHECK_FALSE(model.confirmingRestoreAll());

    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    static_cast<void>(model.apply(UiCommand::Up));
    CHECK_FALSE(model.confirmingRestoreAll());

    static_cast<void>(model.apply(UiCommand::Down));
    CHECK_FALSE(model.apply(UiCommand::Activate).has_value());
    const auto command = model.apply(UiCommand::Activate);
    REQUIRE(command.has_value());
    CHECK(command->kind == SettingsCommand::Kind::RestoreAll);
}

TEST_CASE("ui/settings: Back closes it", "[ui][settings]") {
    SettingsModel model;
    model.setRows(table());
    model.open();
    const auto command = model.apply(UiCommand::Back);
    REQUIRE(command.has_value());
    CHECK(command->kind == SettingsCommand::Kind::Close);
    CHECK_FALSE(model.isOpen());
}

TEST_CASE("ui/settings: rows given again keep the focus on its row", "[ui][settings]") {
    SettingsModel model;
    model.setRows(table());
    model.open();
    static_cast<void>(model.apply(UiCommand::Down));
    std::vector<SettingsRow> rows = table();
    rows.erase(rows.begin()); // hidden in a headset
    model.setRows(rows);
    REQUIRE(model.shown().size() == 1);
    CHECK(model.focus() == 0);
    CHECK(model.shown()[0]->key == "fov");
}

TEST_CASE("ui/settings: a group that lost its rows gives way", "[ui][settings]") {
    SettingsModel model;
    model.setRows(table());
    model.open();
    std::vector<SettingsRow> rows = table();
    rows.erase(rows.begin(), rows.begin() + 2);
    model.setRows(rows);
    CHECK(model.group() == "audio");
}

TEST_CASE("ui/settings: clicks mirror the keys", "[ui][settings]") {
    SettingsModel model;
    std::vector<SettingsRow> rows = table();
    rows[2].overridden = true;
    model.setRows(rows);
    model.open();

    CHECK_FALSE(model.click(SettingsClick{SettingsClick::Kind::Tab, 1, 1}).has_value());
    CHECK(model.group() == "audio");
    // The second row: focused and flipped.
    CHECK(model.click(SettingsClick{SettingsClick::Kind::Row, 1, 1}) == change("mute", true));
    CHECK(model.focus() == 1);
    // A slider's row is only focused; its arrows step.
    CHECK_FALSE(model.click(SettingsClick{SettingsClick::Kind::Row, 0, 1}).has_value());
    CHECK(model.focus() == 0);
    CHECK(model.click(SettingsClick{SettingsClick::Kind::Step, 0, -1}) == change("master", 0.95));
    const auto restored = model.click(SettingsClick{SettingsClick::Kind::Restore, 0, 1});
    REQUIRE(restored.has_value());
    CHECK(restored->kind == SettingsCommand::Kind::Restore);
    CHECK_FALSE(model.click(SettingsClick{SettingsClick::Kind::Row, 9, 1}).has_value());

    CHECK_FALSE(model.click(SettingsClick{SettingsClick::Kind::RestoreAll, 0, 1}).has_value());
    CHECK(model.confirmingRestoreAll());
    const auto all = model.click(SettingsClick{SettingsClick::Kind::RestoreAll, 0, 1});
    REQUIRE(all.has_value());
    CHECK(all->kind == SettingsCommand::Kind::RestoreAll);

    // A group without rows takes no click.
    CHECK_FALSE(model.click(SettingsClick{SettingsClick::Kind::Tab, 3, 1}).has_value());
    CHECK(model.group() == "audio");
}
