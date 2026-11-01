#include <rv/ui/hint_bar.hpp>

#include <cstddef>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace rv::ui {
namespace {

/// A pad's face button: its colour class, the round glyph.
[[nodiscard]] std::string colourOf(const input::Binding& binding) {
    const auto* button = std::get_if<input::GamepadButtonBinding>(&binding);
    if (button == nullptr) {
        return {};
    }
    switch (button->button) {
    case input::GamepadButton::South:
        return "pad-a";
    case input::GamepadButton::East:
        return "pad-b";
    case input::GamepadButton::West:
        return "pad-x";
    case input::GamepadButton::North:
        return "pad-y";
    default:
        return {};
    }
}

} // namespace

void HintBar::setLabels(input::KeyLabels labels) {
    m_labels = std::move(labels);
    ++m_revision;
}

void HintBar::setMap(input::ActionMap map) {
    if (map != m_map) {
        m_map = std::move(map);
        ++m_revision;
    }
}

void HintBar::setDevice(input::DeviceKind device) noexcept {
    if (device != m_device) {
        m_device = device;
        ++m_revision;
    }
}

std::string HintBar::label(input::Action action, input::DeviceKind device) const {
    return input::bindingLabel(m_map, action, device, m_labels);
}

std::string HintBar::promptLabel(input::Action action, input::DeviceKind device) const {
    std::string name = label(action, device);
    if (name.empty()) {
        name = label(action, device == input::DeviceKind::Gamepad ? input::DeviceKind::KeyboardMouse
                                                                  : input::DeviceKind::Gamepad);
    }
    return name;
}

HintGlyph HintBar::glyph(std::string_view actions) const {
    std::vector<input::Binding> bindings;
    std::size_t start = 0;
    while (start < actions.size()) {
        const std::size_t space = actions.find(' ', start);
        const std::string_view name =
            actions.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start);
        start = space == std::string_view::npos ? actions.size() : space + 1;
        const std::optional<input::Action> action = input::actionFromName(name);
        if (!action) {
            continue;
        }
        const std::optional<input::Binding> binding = input::primaryBinding(m_map, *action, m_device);
        if (binding && (bindings.empty() || !input::sameInput(bindings.back(), *binding))) {
            bindings.push_back(*binding);
        }
    }
    HintGlyph glyph;
    if (bindings.empty()) {
        return glyph;
    }
    glyph.bound = true;
    if (bindings.size() == 1) {
        glyph.text = m_labels.label(bindings.front());
        glyph.colour = colourOf(bindings.front());
        glyph.wide = glyph.colour.empty();
        return glyph;
    }
    std::string codes;
    std::string names;
    for (const input::Binding& binding : bindings) {
        codes += (codes.empty() ? "" : " ") + std::string(input::bindingCode(binding));
        names += (names.empty() ? "" : " ") + m_labels.label(binding);
    }
    const std::string group = m_labels.group(codes);
    glyph.text = group.empty() ? names : group;
    glyph.wide = true;
    return glyph;
}

} // namespace rv::ui
