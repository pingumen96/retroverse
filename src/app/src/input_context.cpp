#include <rv/app/input_context.hpp>

#include <cstddef>

namespace rv::app {

std::string_view toString(InputContext context) noexcept {
    switch (context) {
    case InputContext::World:
        return "world";
    case InputContext::Ui:
        return "ui";
    }
    return "unknown";
}

bool readsAction(InputContext context, input::Action action) noexcept {
    if (isGlobalAction(action)) {
        return true;
    }
    switch (context) {
    case InputContext::World:
        return !input::isUiAction(action);
    case InputContext::Ui:
        return input::isUiAction(action) || action == input::Action::QuickCatalog || action == input::Action::Map;
    }
    return false;
}

input::ActionState actionsFor(InputContext context, const input::ActionState& actions) noexcept {
    input::ActionState read = actions;
    for (std::size_t index = 0; index < input::kActionCount; ++index) {
        const auto action = static_cast<input::Action>(index);
        if (!readsAction(context, action)) {
            read.setButton(action, false, false, false);
            read.setAxis(action, 0.0f);
        }
    }
    return read;
}

bool anyUiActionPressed(const input::ActionState& actions) noexcept {
    for (std::size_t index = 0; index < input::kActionCount; ++index) {
        const auto action = static_cast<input::Action>(index);
        if (input::isUiAction(action) && actions.pressed(action)) {
            return true;
        }
    }
    return false;
}

bool catalogToggleRequested(InputContext context, const input::ActionState& actions) noexcept {
    if (!actions.pressed(input::Action::QuickCatalog)) {
        return false;
    }
    return context == InputContext::World || !anyUiActionPressed(actions);
}

} // namespace rv::app
