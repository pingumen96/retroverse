#include <rv/input/binding.hpp>

#include <cmath>

namespace rv::input {
namespace {

struct BindingValidator {
    bool operator()(const KeyBinding& binding) const noexcept {
        return toIndex(binding.key) < kKeyCount && std::isfinite(binding.scale);
    }
    bool operator()(const MouseButtonBinding& binding) const noexcept {
        return toIndex(binding.button) < kMouseButtonCount && std::isfinite(binding.scale);
    }
    bool operator()(const MouseAxisBinding& binding) const noexcept {
        return toIndex(binding.axis) < kMouseAxisCount && std::isfinite(binding.scale);
    }
    bool operator()(const GamepadButtonBinding& binding) const noexcept {
        return toIndex(binding.button) < kGamepadButtonCount && std::isfinite(binding.scale);
    }
    bool operator()(const GamepadAxisBinding& binding) const noexcept {
        // Written so that NaN fails every comparison.
        return toIndex(binding.axis) < kGamepadAxisCount && std::isfinite(binding.scale) &&
               binding.deadzone >= 0.0f && binding.deadzone < 1.0f && std::isfinite(binding.exponent) &&
               binding.exponent > 0.0f;
    }
};

} // namespace

bool isValid(const Binding& binding) noexcept {
    return std::visit(BindingValidator{}, binding);
}

} // namespace rv::input
