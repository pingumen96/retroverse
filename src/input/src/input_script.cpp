#include <rv/input/input_script.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/input/input_system.hpp>

#include "json_location.hpp"

namespace rv::input {
namespace {

using Json = nlohmann::json;
using detail::fail;
using detail::inQuotes;
using detail::JsonLocation;

constexpr std::string_view kFromFrameField = "fromFrame";
constexpr std::string_view kToFrameField = "toFrame";
constexpr std::string_view kActionsField = "actions";
constexpr std::array<std::string_view, 3> kStepFields{kFromFrameField, kToFrameField, kActionsField};

Expected<std::uint64_t> parseFrame(const Json& step, std::string_view field, const JsonLocation& stepAt) {
    const JsonLocation at = stepAt.child(field);
    const auto it = step.find(field);
    if (it == step.end()) {
        return fail(ErrorCode::ParseError, "missing field " + inQuotes(field), at);
    }
    if (!it->is_number_integer()) {
        return fail(ErrorCode::ParseError, "expected a frame index (an integer)", at);
    }
    std::uint64_t frame = 0;
    if (it->is_number_unsigned()) {
        frame = it->get<std::uint64_t>();
    } else {
        const auto signedFrame = it->get<std::int64_t>();
        if (signedFrame < 0) {
            return fail(ErrorCode::ValidationFailed, "frames are counted from 0", at);
        }
        frame = static_cast<std::uint64_t>(signedFrame);
    }
    if (frame > InputScript::kMaxFrame) {
        return fail(ErrorCode::ValidationFailed,
                    "frame " + std::to_string(frame) + " is beyond the last possible frame " +
                        std::to_string(InputScript::kMaxFrame),
                    at);
    }
    return frame;
}

Expected<float> parseValue(Action action, const Json& value, const JsonLocation& at) {
    if (!value.is_number()) {
        return fail(ErrorCode::ParseError, "expected a number", at);
    }
    const auto number = static_cast<float>(value.get<double>());
    if (!std::isfinite(number)) {
        return fail(ErrorCode::ValidationFailed, "number out of range", at);
    }
    const bool unbounded = action == Action::LookDeltaX || action == Action::LookDeltaY;
    if (!unbounded && (number < -1.0f || number > 1.0f)) {
        return fail(ErrorCode::ValidationFailed,
                    inQuotes(actionName(action)) +
                        " takes values in [-1, 1] (only look_delta_x and look_delta_y take mouse pixels)",
                    at);
    }
    return number;
}

Expected<InputScriptStep> parseStep(const Json& step, const JsonLocation& at) {
    if (!step.is_object()) {
        return fail(ErrorCode::ParseError,
                    "expected a step object such as {\"fromFrame\": 0, \"toFrame\": 59, \"actions\": "
                    "{\"move_forward\": 1}}",
                    at);
    }
    for (const auto& item : step.items()) {
        if (std::find(kStepFields.begin(), kStepFields.end(), item.key()) == kStepFields.end()) {
            return fail(ErrorCode::ParseError, "unknown field " + inQuotes(item.key()), at.child(item.key()));
        }
    }

    InputScriptStep parsed;
    const auto from = parseFrame(step, kFromFrameField, at);
    if (!from) {
        return propagate(from.error());
    }
    const auto to = parseFrame(step, kToFrameField, at);
    if (!to) {
        return propagate(to.error());
    }
    if (*to < *from) {
        return fail(ErrorCode::ValidationFailed,
                    "toFrame " + std::to_string(*to) + " comes before fromFrame " + std::to_string(*from) +
                        " (both frames are included: a single frame has fromFrame == toFrame)",
                    at.child(kToFrameField));
    }
    parsed.fromFrame = *from;
    parsed.toFrame = *to;

    const JsonLocation actionsAt = at.child(kActionsField);
    const auto actions = step.find(kActionsField);
    if (actions == step.end()) {
        return fail(ErrorCode::ParseError, "missing field \"actions\"", actionsAt);
    }
    if (!actions->is_object()) {
        return fail(ErrorCode::ParseError, "expected an object mapping action names to values", actionsAt);
    }
    if (actions->empty()) {
        return fail(ErrorCode::ValidationFailed, "a step must set at least one action", actionsAt);
    }
    for (const auto& item : actions->items()) {
        const JsonLocation valueAt = actionsAt.child(item.key());
        const auto action = actionFromName(item.key());
        if (!action) {
            return fail(ErrorCode::ValidationFailed,
                        "unknown action " + inQuotes(item.key()) +
                            " (names as in binding files: \"move_forward\", \"look_delta_x\", ...)",
                        valueAt);
        }
        const auto value = parseValue(*action, item.value(), valueAt);
        if (!value) {
            return propagate(value.error());
        }
        parsed.values.emplace_back(*action, *value);
    }
    return parsed;
}

[[nodiscard]] float sanitizedThreshold(float threshold) noexcept {
    const float value = std::isfinite(threshold) ? threshold : InputSettings{}.pressThreshold;
    return std::clamp(value, kMinPressThreshold, 1.0f);
}

} // namespace

Expected<InputScript> InputScript::fromJson(const nlohmann::json& document, std::string_view context) {
    const JsonLocation root(context);
    if (!document.is_array()) {
        return fail(ErrorCode::ParseError,
                    "expected an array of steps such as [{\"fromFrame\": 0, \"toFrame\": 59, \"actions\": "
                    "{\"move_forward\": 1}}]",
                    root);
    }
    if (document.empty()) {
        return fail(ErrorCode::ValidationFailed, "the script has no steps", root);
    }
    if (document.size() > kMaxSteps) {
        return fail(ErrorCode::ValidationFailed,
                    "too many steps: " + std::to_string(document.size()) + " (at most " + std::to_string(kMaxSteps) +
                        ")",
                    root);
    }
    InputScript script;
    script.m_steps.reserve(document.size());
    for (std::size_t index = 0; index < document.size(); ++index) {
        auto step = parseStep(document[index], root.child(index));
        if (!step) {
            return propagate(std::move(step.error()));
        }
        script.m_steps.push_back(std::move(*step));
    }
    return script;
}

Expected<InputScript> InputScript::loadFromFile(const std::filesystem::path& path) {
    const auto document = core::readJsonFile(path);
    if (!document) {
        return propagate(document.error());
    }
    return fromJson(*document, core::pathToUtf8(path));
}

std::uint64_t InputScript::lastFrame() const noexcept {
    std::uint64_t last = 0;
    for (const InputScriptStep& step : m_steps) {
        last = std::max(last, step.toFrame);
    }
    return last;
}

std::optional<float> InputScript::valueAt(std::uint64_t frame, Action action) const noexcept {
    for (auto step = m_steps.rbegin(); step != m_steps.rend(); ++step) {
        if (!step->covers(frame)) {
            continue;
        }
        for (const auto& [scripted, value] : step->values) {
            if (scripted == action) {
                return value;
            }
        }
    }
    return std::nullopt;
}

InputScriptPlayer::InputScriptPlayer(InputScript script) : m_script(std::move(script)) {}

void InputScriptPlayer::apply(std::uint64_t frame, ActionState& state, float pressThreshold) {
    const float threshold = sanitizedThreshold(pressThreshold);
    for (std::size_t index = 0; index < kActionCount; ++index) {
        const auto action = static_cast<Action>(index);
        const bool wasHeld = m_previousHeld[index];
        float value = state.axis(action);
        bool held = state.held(action);
        // InputSystem reports a button pressed and released within one frame as both edges while not held.
        bool tapped = !held && state.pressed(action) && state.released(action);
        if (const std::optional<float> scripted = m_script.valueAt(frame, action)) {
            value = *scripted;
            held = !isLookAction(action) && std::abs(value) >= threshold;
            tapped = false;
        }
        state.setButton(action, held, !wasHeld && (held || tapped), !held && (wasHeld || tapped));
        state.setAxis(action, value);
        m_previousHeld[index] = held;
    }
}

} // namespace rv::input
