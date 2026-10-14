#pragma once

#include <bitset>

#include <rv/input/action.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/raw_input.hpp>

namespace rv::input {

/// User-adjustable input behavior (specification §12.4: sensitivity, invert). InputSystem::setSettings()
/// replaces non-finite values with these defaults and clamps pressThreshold to [kMinPressThreshold, 1].
struct InputSettings {
    /// |value| at or above which an analog action counts as held (and produces pressed/released edges).
    float pressThreshold = 0.5f;
    /// Negates LookY and LookDeltaY: pushing the stick or moving the mouse up looks down.
    bool invertLookY = false;
    /// Multiplies LookDeltaX/LookDeltaY (mouse, unclamped).
    float mouseSensitivityScale = 1.0f;
    /// Gain on LookX/LookY, applied before their clamp so they stay rates in [-1, 1] (action.hpp): below 1 it
    /// lowers the top rate, above 1 the stick reaches the top rate before full deflection. The turn speed at
    /// rate 1 belongs to the consumer.
    float stickSensitivityScale = 1.0f;
};

/// Smallest accepted press threshold: 0 would hold every axis-bound action at rest.
inline constexpr float kMinPressThreshold = 0.01f;

/// Turns one RawInputFrame into an ActionState through an ActionMap. Deterministic and GPU/SDL-free; its only
/// state between frames is the held flag of each action, used for edge detection. Main thread, one resolve()
/// per frame.
///
/// Analog value of an action: the sum of the contributions of its bindings (see binding.hpp):
///  - held key / mouse button / gamepad button: + scale;
///  - mouse axis: + delta * scale;
///  - gamepad axis: + shape(value) * scale (dead zone with rescaling, then response exponent keeping the sign).
/// The sum is then post-processed per action:
///  - LookDeltaX / LookDeltaY: not clamped (mouse pixels), times mouseSensitivityScale;
///  - LookX / LookY: times stickSensitivityScale, then clamped to [-1, 1] (rates, see action.hpp);
///  - every other action: clamped to [-1, 1];
///  - invertLookY negates LookY and LookDeltaY.
/// MoveForward and MoveRight are resolved independently and the movement vector is NOT normalized here (W + D
/// gives a length of sqrt(2)): the first-person controller normalizes it. Non-finite raw values count as 0.
/// Gamepad fields are ignored while RawInputFrame::gamepadConnected is false.
///
/// Digital state: held when any bound key or button is held, or when |value| >= pressThreshold; LookX, LookY,
/// LookDeltaX and LookDeltaY are never held from analog motion (only from bound buttons). pressed/released are
/// edges against the previous resolve(); a bound key or button pressed and released within one frame (a tap
/// during a long frame) reports both pressed and released. The ActionState axis carries the analog value
/// (1 for a plain held button). activeDevice is copied from the frame.
class InputSystem {
public:
    explicit InputSystem(ActionMap map = ActionMap::defaults());

    /// Replaces the bindings. Edge memory is kept: an action no longer held reports released on the next frame.
    void setMap(ActionMap map);
    [[nodiscard]] const ActionMap& map() const noexcept { return m_map; }

    void setSettings(const InputSettings& settings);
    [[nodiscard]] const InputSettings& settings() const noexcept { return m_settings; }

    /// Resolves one frame and advances the edge memory.
    [[nodiscard]] ActionState resolve(const RawInputFrame& frame);

    /// Forgets which actions were held (focus loss): the next resolve() reports pressed for every action held
    /// at that time and released for none.
    void reset() noexcept;

private:
    ActionMap m_map;
    InputSettings m_settings;
    std::bitset<kActionCount> m_previousHeld;
};

} // namespace rv::input
