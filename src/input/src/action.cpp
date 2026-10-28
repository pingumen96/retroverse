#include <rv/input/action.hpp>

#include <array>

namespace rv::input {
namespace {

constexpr std::array<std::string_view, kActionCount> kNames = {
    "move_forward",
    "move_right",
    "look_x",
    "look_y",
    "look_delta_x",
    "look_delta_y",
    "sprint",
    "interact",
    "back",
    "quick_catalog",
    "confirm",
    "map",
    "menu",
    "toggle_dev_ui",
    "reload_shaders",
    "screenshot",
    "ui_up",
    "ui_down",
    "ui_left",
    "ui_right",
    "ui_nav_x",
    "ui_nav_y",
    "ui_page_up",
    "ui_page_down",
    "ui_home",
    "ui_end",
    "ui_activate",
    "ui_back",
    "ui_media",
    "ui_info",
    "ui_filter_previous",
    "ui_filter_next",
    "toggle_mute",
    "edit_pin",
    "edit_rotate_left",
    "edit_rotate_right",
};
// Fails to compile when an Action is added without a name.
static_assert(!kNames.back().empty(), "every Action needs a name in kNames");

} // namespace

std::string_view actionName(Action action) noexcept {
    const std::size_t index = toIndex(action);
    return index < kNames.size() ? kNames[index] : std::string_view("unknown");
}

std::optional<Action> actionFromName(std::string_view name) noexcept {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (kNames[i] == name) {
            return static_cast<Action>(i);
        }
    }
    return std::nullopt;
}

} // namespace rv::input
