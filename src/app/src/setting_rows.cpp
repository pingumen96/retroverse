#include <rv/app/setting_rows.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <rv/app/binding_rules.hpp>

namespace rv::app {
namespace {

using Json = nlohmann::json;

[[nodiscard]] SettingRow toggle(std::string_view section, std::string_view key, SettingGroup group) {
    SettingRow row;
    row.section = section;
    row.key = key;
    row.group = group;
    row.kind = SettingKind::Toggle;
    return row;
}

[[nodiscard]] SettingRow choice(std::string_view section, std::string_view key, SettingGroup group,
                                std::vector<Json> values) {
    SettingRow row;
    row.section = section;
    row.key = key;
    row.group = group;
    row.kind = SettingKind::Choice;
    row.choices = std::move(values);
    return row;
}

[[nodiscard]] SettingRow slider(std::string_view section, std::string_view key, SettingGroup group, double min,
                                double max, double step) {
    SettingRow row;
    row.section = section;
    row.key = key;
    row.group = group;
    row.kind = SettingKind::Slider;
    row.min = min;
    row.max = max;
    row.step = step;
    return row;
}

[[nodiscard]] SettingRow hiddenInVr(SettingRow row) {
    row.hiddenInVr = true;
    return row;
}

[[nodiscard]] SettingRow committing(SettingRow row) {
    row.apply = SettingApply::CommitFloor;
    return row;
}

[[nodiscard]] SettingRow nextSession(SettingRow row) {
    row.apply = SettingApply::NextSession;
    return row;
}

} // namespace

std::string_view toString(SettingGroup group) noexcept {
    switch (group) {
    case SettingGroup::Display:
        return "display";
    case SettingGroup::Audio:
        return "audio";
    case SettingGroup::World:
        return "world";
    case SettingGroup::Controls:
        return "controls";
    case SettingGroup::Vr:
        return "vr";
    }
    return "unknown";
}

std::string SettingRow::id() const {
    return std::string(section) + "." + std::string(key);
}

bool SettingRow::takes(const Json& value) const {
    switch (kind) {
    case SettingKind::Toggle:
        return value.is_boolean();
    case SettingKind::Choice:
        return std::find(choices.begin(), choices.end(), value) != choices.end();
    case SettingKind::Slider:
        if (value.is_null()) {
            return nullable;
        }
        return value.is_number() && std::isfinite(value.get<double>()) && value.get<double>() >= min &&
               value.get<double>() <= max;
    case SettingKind::Binding:
        return value.is_array();
    case SettingKind::Action:
        return false;
    }
    return false;
}

std::vector<SettingRow> settingRows(std::span<const std::string> lightingProfiles) {
    constexpr std::string_view kPlayer = "player";
    constexpr std::string_view kVideo = "video";
    constexpr std::string_view kWorld = "world";
    constexpr std::string_view kXr = "xr";

    std::vector<SettingRow> rows;
    // Schermo.
    rows.push_back(hiddenInVr(choice(kPlayer, "displayMode", SettingGroup::Display, {"window", "borderless"})));
    rows.push_back(choice(kPlayer, "textScale", SettingGroup::Display, {1.0, 1.25}));
    SettingRow fov = hiddenInVr(slider(kPlayer, "horizontalFovDeg", SettingGroup::Display, 70.0, 110.0, 5.0));
    fov.nullable = true;
    rows.push_back(std::move(fov));
    // Audio.
    rows.push_back(slider(kVideo, "audio.master", SettingGroup::Audio, 0.0, 1.0, 0.05));
    rows.push_back(slider(kVideo, "audio.video", SettingGroup::Audio, 0.0, 1.0, 0.05));
    rows.push_back(toggle(kVideo, "audio.muted", SettingGroup::Audio));
    rows.push_back(choice(kVideo, "audio.mode", SettingGroup::Audio, {"nearest-focused", "off"}));
    // Mondo.
    std::vector<Json> profiles(lightingProfiles.begin(), lightingProfiles.end());
    rows.push_back(committing(choice(kWorld, "lightingProfile", SettingGroup::World, std::move(profiles))));
    rows.push_back(toggle(kWorld, "reduceMotion", SettingGroup::World));
    rows.push_back(toggle(kVideo, "enabled", SettingGroup::World));
    rows.push_back(committing(choice(kVideo, "crtEffect", SettingGroup::World, {"off", "light", "vintage"})));
    rows.push_back(
        choice(kVideo, "hero.playlist", SettingGroup::World, {"shuffle", "sequential", "favorites", "recent"}));
    // Controlli.
    rows.push_back(slider(kPlayer, "mouseSensitivity", SettingGroup::Controls, 0.25, 3.0, 0.25));
    rows.push_back(slider(kPlayer, "stickSensitivity", SettingGroup::Controls, 0.25, 3.0, 0.25));
    rows.push_back(toggle(kPlayer, "invertY", SettingGroup::Controls));
    for (const BindableAction& bindable : bindableActions()) {
        SettingRow row;
        row.section = kBindingsSettingsSection;
        row.key = input::actionName(bindable.action);
        row.group = SettingGroup::Controls;
        row.kind = SettingKind::Binding;
        rows.push_back(std::move(row));
    }
    SettingRow restoreBindings;
    restoreBindings.section = kBindingsSettingsSection;
    restoreBindings.key = kRestoreBindingsKey;
    restoreBindings.group = SettingGroup::Controls;
    restoreBindings.kind = SettingKind::Action;
    rows.push_back(std::move(restoreBindings));
    // VR: each read when a session starts.
    rows.push_back(nextSession(choice(kXr, "display.mode", SettingGroup::Vr, {"desktop", "vr"})));
    rows.push_back(nextSession(choice(kXr, "comfort.turn", SettingGroup::Vr, {"snap", "smooth"})));
    rows.push_back(nextSession(slider(kXr, "comfort.vignette", SettingGroup::Vr, 0.0, 1.0, 0.1)));
    rows.push_back(nextSession(choice(kXr, "comfort.moveRelativeTo", SettingGroup::Vr, {"head", "hand"})));
    rows.push_back(nextSession(choice(kXr, "posture", SettingGroup::Vr, {"standing", "seated"})));
    rows.push_back(nextSession(choice(kXr, "quality", SettingGroup::Vr, {"auto", "full", "fast"})));
    rows.push_back(nextSession(choice(kXr, "refreshRate", SettingGroup::Vr, {0, 72, 90, 120})));
    return rows;
}

} // namespace rv::app
