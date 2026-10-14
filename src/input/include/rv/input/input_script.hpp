#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <rv/core/expected.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_state.hpp>

namespace rv::input {

/// One step of an input script: on every frame from fromFrame to toFrame (both included) the listed actions take the
/// given values.
struct InputScriptStep {
    std::uint64_t fromFrame = 0;
    std::uint64_t toFrame = 0;
    /// Scripted (action, value) pairs, ordered by action name (the order of a parsed JSON object).
    std::vector<std::pair<Action, float>> values;

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
///  - "actions": at least one entry keyed by actionName(). look_delta_x and look_delta_y take any finite number (mouse
///    pixels this frame, see Action); every other action takes a number in [-1, 1], the range InputSystem produces
///    (1 holds a button).
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

private:
    std::vector<InputScriptStep> m_steps;
};

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

private:
    InputScript m_script;
    std::bitset<kActionCount> m_previousHeld;
};

} // namespace rv::input
