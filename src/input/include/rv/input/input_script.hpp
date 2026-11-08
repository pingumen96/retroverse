#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <nlohmann/json_fwd.hpp>

#include <rv/core/expected.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/codes.hpp>
#include <rv/input/raw_input.hpp>

namespace rv::input {

/// One step of an input script: on every frame from fromFrame to toFrame (both included) the listed actions take the
/// given values.
/// A head pose an input script gives the VR mode's fake runtime (I10 step 2): where the head looks, degrees (yaw and
/// pitch as the camera's: 0 looks at -Z, positive yaw turns right and positive pitch looks up; positive roll tilts the
/// head to the right), and where it is in the tracking space, meters (nothing: the fake runtime's standing head).
struct ScriptedHeadPose {
    /// Largest |coordinate| accepted, meters.
    static constexpr float kMaxCoordinate = 100.0f;

    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
    float rollDeg = 0.0f;
    std::optional<glm::vec3> position;

    bool operator==(const ScriptedHeadPose&) const = default;
};

/// What a step holds on the devices themselves (I7 step 3), before the bindings resolve it: unlike "actions", it goes
/// through the map, so a rebound action can be proven, and it is what a capture of the settings screen reads.
struct ScriptedRawInput {
    std::vector<Key> keys;
    std::vector<GamepadButton> padButtons;
    /// Pad axes in their ranges (codes.hpp), [-1, 1] or [0, 1].
    std::vector<std::pair<GamepadAxis, float>> padAxes;
    /// A pad plugged in (true), or none at all (false); nothing: as the devices say. Holding a pad's button or axis
    /// plugs one in.
    std::optional<bool> padConnected;

    bool operator==(const ScriptedRawInput&) const = default;
};

struct InputScriptStep {
    std::uint64_t fromFrame = 0;
    std::uint64_t toFrame = 0;
    /// Scripted (action, value) pairs, ordered by action name (the order of a parsed JSON object).
    std::vector<std::pair<Action, float>> values;
    /// Text typed on fromFrame, once (UTF-8): what the product UI's focused text field receives, as if typed on a
    /// keyboard (the quick catalog's search). Empty: none.
    std::string text;
    /// A named phase of the run (app::FrameTiming) open on every frame of the step: its frame times are reported apart
    /// (the run report's phases). Empty: none.
    std::string phase;
    /// The head's pose on every frame of the step, for the VR mode's fake runtime (ScriptedHeadPose). Nothing: none.
    std::optional<ScriptedHeadPose> headPose;
    /// The left and right controllers' poses (I10 step 4), in the same terms; nothing: that hand holds none.
    std::optional<ScriptedHeadPose> leftHand;
    std::optional<ScriptedHeadPose> rightHand;
    /// Their triggers on every frame of the step (I10 step 5: "leftSelect", "rightSelect", [0, 1]); nothing: released.
    std::optional<float> leftSelect;
    std::optional<float> rightSelect;
    /// Their thumbsticks on every frame of the step (I10 step 10: "leftStick", "rightStick", [x, y] in [-1, 1], +y
    /// pushed forward); nothing: centered.
    std::optional<glm::vec2> leftStick;
    std::optional<glm::vec2> rightStick;
    /// I11 step 14: the left controller's menu button held on every frame of the step ("leftMenu": true).
    bool leftMenu = false;
    /// I7 step 3: the devices' state on every frame of the step ("raw").
    std::optional<ScriptedRawInput> raw;
    /// I7 step 3: the device used last from this step on ("device": "pad", "keys"), until another step says.
    std::optional<DeviceKind> device;

    [[nodiscard]] bool covers(std::uint64_t frame) const noexcept { return frame >= fromFrame && frame <= toFrame; }
};

/// Deterministic input automation (retroverse.exe --input-script): steps whose values replace the action state the
/// devices produced on chosen frames, so an automated run walks, turns and presses buttons reproducibly (together
/// with --fixed-dt). Frames are counted from 0, the first frame of the run.
///
/// JSON format (comments allowed): a top-level array of steps,
///
///     [
///       {"fromFrame": 0, "toFrame": 119, "actions": {"move_forward": 1.0}},
///       {"fromFrame": 120, "toFrame": 129, "actions": {"look_delta_x": 20.0, "sprint": 1}}
///     ]
///
///  - "fromFrame", "toFrame": integers with 0 <= fromFrame <= toFrame <= kMaxFrame; both frames are included.
///  - "actions": entries keyed by actionName(). look_delta_x and look_delta_y take any finite number (mouse pixels this
///    frame, see Action); every other action takes a number in [-1, 1], the range InputSystem produces (1 holds a
///    button).
///  - "text" (I2 step 13): a string typed once, on fromFrame, into the product UI's focused text field (the quick
///    catalog's search: {"fromFrame": 9, "toFrame": 9, "text": "nintendo"}); valid UTF-8, 1 to kMaxTextBytes bytes,
///    no control characters.
///  - "phase" (I3 step 0d): the name of a phase of the run open from fromFrame to toFrame, whose frame times the run
///    report gives apart ({"fromFrame": 40, "toFrame": 99, "phase": "benchmark"}): an ASCII letter, then up to
///    kMaxPhaseNameBytes - 1 ASCII letters, digits, '_' or '-' (isValidPhaseName). Steps with the same phase that
///    overlap or touch keep it open throughout.
///  - "headPose" (I10 step 2): the head's pose for the VR mode's fake runtime on every frame of the step,
///    {"yaw": 30, "pitch": -10, "roll": 0, "position": [0, 1.65, -0.2]}: degrees and meters (ScriptedHeadPose), every
///    field optional (angles 0, the fake runtime's standing head), finite, |pitch| at most 90 and |coordinates| at
///    most ScriptedHeadPose::kMaxCoordinate.
///  - "leftHand", "rightHand" (I10 step 4): a controller's pose in that hand, in the same terms as "headPose" (it
///    points along its -Z; no position: the fake runtime's resting hand); "leftSelect", "rightSelect" (I10 step 5)
///    its trigger, a number in [0, 1]; "leftStick", "rightStick" (I10 step 10) its thumbstick, [x, y] in [-1, 1] with
///    +y pushed forward; "leftMenu" (I11 step 14) the left controller's menu button held, a boolean.
///  - "raw" (I7 step 3): the devices themselves on every frame of the step, before the bindings:
///    {"keys": ["insert"], "padButtons": ["west"], "padAxes": {"left_trigger": 1}, "padConnected": true}, every field
///    optional (ScriptedRawInput). A press through it makes its device the one used last, as a real press does.
///  - "device" (I7 step 3): "pad" or "keys", the device used last from fromFrame on, until another step says: the
///    hints' device without any press.
/// A step has at least one of "actions", "text", "phase", "headPose", "leftHand", "rightHand", "raw" and "device", and
/// an "actions" it has is not empty.
///  - Steps may overlap. On a frame where several steps set the same action, the step that comes later in the file
///    wins.
/// Every field is required, unknown fields are errors and a script holds 1 to kMaxSteps steps. Error contexts are
/// "<context>:<JSON pointer>", e.g. "walk.json:/0/actions/move_forwad". Error codes: ParseError (wrong JSON type,
/// missing or unknown field), ValidationFailed (unknown action, value or frame out of range, toFrame < fromFrame, no
/// steps or no actions, too many steps).
class InputScript {
public:
    /// Largest frame index a step may name: the largest frame limit of a run (--frames is a 32-bit count).
    static constexpr std::uint64_t kMaxFrame = 0xFFFFFFFFu;
    /// Bound on the number of steps, so a hostile file cannot make every frame slow.
    static constexpr std::size_t kMaxSteps = 10000;
    /// Longest "text" of a step, in bytes (the quick catalog's search field holds 64 characters).
    static constexpr std::size_t kMaxTextBytes = 256;
    /// Longest "phase" of a step, in bytes.
    static constexpr std::size_t kMaxPhaseNameBytes = 64;

    /// Parses a script document; `context` prefixes error locations (usually the file name).
    [[nodiscard]] static Expected<InputScript> fromJson(const nlohmann::json& document, std::string_view context);
    /// Reads (rv::core::readJsonFile) and parses a script file, with the UTF-8 path as the error context.
    [[nodiscard]] static Expected<InputScript> loadFromFile(const std::filesystem::path& path);

    [[nodiscard]] std::span<const InputScriptStep> steps() const noexcept { return m_steps; }
    /// The last frame any step covers.
    [[nodiscard]] std::uint64_t lastFrame() const noexcept;
    /// Scripted value of `action` on `frame`: the value of the last step in file order that covers the frame and sets
    /// the action, or nothing.
    [[nodiscard]] std::optional<float> valueAt(std::uint64_t frame, Action action) const noexcept;
    /// The text typed on `frame`: the "text" of every step whose fromFrame it is, in file order, joined; empty when
    /// none.
    [[nodiscard]] std::string textAt(std::uint64_t frame) const;
    /// The phases open on `frame`: the "phase" of every step that covers it, sorted, without repeats.
    [[nodiscard]] std::vector<std::string> phasesAt(std::uint64_t frame) const;
    /// The head pose on `frame`: the "headPose" of the last step in file order that covers it and has one, or nothing.
    [[nodiscard]] std::optional<ScriptedHeadPose> headPoseAt(std::uint64_t frame) const;
    /// A hand's controller on `frame` (the right one when `right`): as headPoseAt() for "rightHand" or "leftHand".
    [[nodiscard]] std::optional<ScriptedHeadPose> handPoseAt(std::uint64_t frame, bool right) const;
    /// A hand's trigger on `frame`: the "rightSelect" or "leftSelect" of the last step that covers it, else 0.
    [[nodiscard]] float handSelectAt(std::uint64_t frame, bool right) const;
    /// A hand's thumbstick on `frame`: the "rightStick" or "leftStick" of the last step that covers it, else centered.
    [[nodiscard]] glm::vec2 handStickAt(std::uint64_t frame, bool right) const;
    /// I11 step 14: the left controller's menu button held on `frame` (a step covering it with "leftMenu": true).
    [[nodiscard]] bool leftMenuAt(std::uint64_t frame) const;

private:
    std::vector<InputScriptStep> m_steps;
};

/// True for a phase name an input script accepts: an ASCII letter, then ASCII letters, digits, '_' or '-', at most
/// InputScript::kMaxPhaseNameBytes in all.
[[nodiscard]] bool isValidPhaseName(std::string_view name) noexcept;

/// Applies an InputScript to the action states of consecutive frames.
class InputScriptPlayer {
public:
    explicit InputScriptPlayer(InputScript script);

    [[nodiscard]] const InputScript& script() const noexcept { return m_script; }

    /// Overrides `state`, the ActionState InputSystem resolved for `frame`. Call once per frame, every frame, in
    /// increasing frame order (frames without scripted values keep the edge memory up to date).
    ///  - Every action the script sets on this frame takes the scripted value as its axis, and is held when
    ///    |value| >= pressThreshold (look actions never are, see isLookAction(); same rule as InputSystem).
    ///  - pressed and released of every action are then the edges of the final held state against the previous
    ///    applied frame, whatever produced it: a scripted button press reports pressed once, the end of a scripted
    ///    hold reports released, and a key tapped within one frame (InputSystem reports pressed and released while
    ///    not held) stays a tap.
    /// `pressThreshold` should be the InputSystem's (InputSettings::pressThreshold); it is clamped to
    /// [kMinPressThreshold, 1], non-finite values counting as the InputSettings default.
    void apply(std::uint64_t frame, ActionState& state, float pressThreshold);
    /// The script's "raw" and "device" over `raw`, before the bindings resolve it. Call once per frame, every frame, in
    /// increasing frame order: edges are against the previous call.
    void applyRaw(std::uint64_t frame, RawInputFrame& raw);
    /// Takes what `state` holds as the previous frame's, so that nothing held reports pressed on the next apply():
    /// after the bindings changed (AppInput::setBindings).
    void noteHeld(const ActionState& state) noexcept;

private:
    InputScript m_script;
    std::bitset<kActionCount> m_previousHeld;
    std::bitset<kKeyCount> m_rawKeys;
    std::bitset<kGamepadButtonCount> m_rawButtons;
    std::optional<DeviceKind> m_device;
};

} // namespace rv::input
