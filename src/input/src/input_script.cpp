#include <rv/input/input_script.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
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
constexpr std::string_view kTextField = "text";
constexpr std::string_view kPhaseField = "phase";
constexpr std::string_view kHeadPoseField = "headPose";
constexpr std::string_view kLeftHandField = "leftHand";
constexpr std::string_view kRightHandField = "rightHand";
constexpr std::string_view kLeftSelectField = "leftSelect";
constexpr std::string_view kRightSelectField = "rightSelect";
constexpr std::array<std::string_view, 10> kStepFields{kFromFrameField,  kToFrameField,   kActionsField,
                                                       kTextField,       kPhaseField,     kHeadPoseField,
                                                       kLeftHandField,   kRightHandField, kLeftSelectField,
                                                       kRightSelectField};
constexpr std::array<std::string_view, 4> kHeadPoseFields{"yaw", "pitch", "roll", "position"};
constexpr float kMaxHeadPitchDeg = 90.0f;

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

Expected<float> parseAngle(const Json& pose, std::string_view field, float limit, const JsonLocation& poseAt) {
    const auto it = pose.find(field);
    if (it == pose.end()) {
        return 0.0f;
    }
    const JsonLocation at = poseAt.child(field);
    if (!it->is_number()) {
        return fail(ErrorCode::ParseError, "expected degrees (a number)", at);
    }
    const auto degrees = static_cast<float>(it->get<double>());
    if (!std::isfinite(degrees) || std::abs(degrees) > limit) {
        return fail(ErrorCode::ValidationFailed, "degrees out of range", at);
    }
    return degrees;
}

Expected<ScriptedHeadPose> parseHeadPose(const Json& pose, const JsonLocation& at) {
    if (!pose.is_object()) {
        return fail(ErrorCode::ParseError,
                    "expected a head pose such as {\"yaw\": 30, \"pitch\": -10, \"position\": [0, 1.65, 0]}", at);
    }
    for (const auto& item : pose.items()) {
        if (std::find(kHeadPoseFields.begin(), kHeadPoseFields.end(), item.key()) == kHeadPoseFields.end()) {
            return fail(ErrorCode::ParseError, "unknown field " + inQuotes(item.key()), at.child(item.key()));
        }
    }
    ScriptedHeadPose parsed;
    const auto yaw = parseAngle(pose, "yaw", std::numeric_limits<float>::max(), at);
    if (!yaw) {
        return propagate(yaw.error());
    }
    const auto pitch = parseAngle(pose, "pitch", kMaxHeadPitchDeg, at);
    if (!pitch) {
        return propagate(pitch.error());
    }
    const auto roll = parseAngle(pose, "roll", std::numeric_limits<float>::max(), at);
    if (!roll) {
        return propagate(roll.error());
    }
    parsed.yawDeg = *yaw;
    parsed.pitchDeg = *pitch;
    parsed.rollDeg = *roll;
    if (const auto position = pose.find("position"); position != pose.end()) {
        const JsonLocation positionAt = at.child("position");
        if (!position->is_array() || position->size() != 3) {
            return fail(ErrorCode::ParseError, "expected [x, y, z] in meters", positionAt);
        }
        glm::vec3 xyz(0.0f);
        for (std::size_t i = 0; i < 3; ++i) {
            const Json& value = (*position)[i];
            if (!value.is_number()) {
                return fail(ErrorCode::ParseError, "expected [x, y, z] in meters", positionAt);
            }
            xyz[static_cast<glm::length_t>(i)] = static_cast<float>(value.get<double>());
            if (!std::isfinite(xyz[static_cast<glm::length_t>(i)]) ||
                std::abs(xyz[static_cast<glm::length_t>(i)]) > ScriptedHeadPose::kMaxCoordinate) {
                return fail(ErrorCode::ValidationFailed, "a coordinate out of range", positionAt);
            }
        }
        parsed.position = xyz;
    }
    return parsed;
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

    const JsonLocation textAt = at.child(kTextField);
    if (const auto text = step.find(kTextField); text != step.end()) {
        if (!text->is_string()) {
            return fail(ErrorCode::ParseError, "expected the text to type (a string)", textAt);
        }
        parsed.text = text->get<std::string>();
        if (parsed.text.empty() || parsed.text.size() > InputScript::kMaxTextBytes) {
            return fail(ErrorCode::ValidationFailed,
                        "the text must hold 1 to " + std::to_string(InputScript::kMaxTextBytes) + " bytes", textAt);
        }
        if (!core::isValidUtf8(parsed.text)) {
            return fail(ErrorCode::ValidationFailed, "the text is not valid UTF-8", textAt);
        }
        const bool control = std::any_of(parsed.text.begin(), parsed.text.end(), [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte < 0x20 || byte == 0x7F;
        });
        if (control) {
            return fail(ErrorCode::ValidationFailed, "the text holds a control character", textAt);
        }
    }

    const JsonLocation phaseAt = at.child(kPhaseField);
    if (const auto phase = step.find(kPhaseField); phase != step.end()) {
        if (!phase->is_string()) {
            return fail(ErrorCode::ParseError, "expected the name of a phase (a string)", phaseAt);
        }
        parsed.phase = phase->get<std::string>();
        if (!isValidPhaseName(parsed.phase)) {
            return fail(ErrorCode::ValidationFailed,
                        "a phase name is an ASCII letter followed by letters, digits, '_' or '-', at most " +
                            std::to_string(InputScript::kMaxPhaseNameBytes) + " in all",
                        phaseAt);
        }
    }

    if (const auto pose = step.find(kHeadPoseField); pose != step.end()) {
        auto headPose = parseHeadPose(*pose, at.child(kHeadPoseField));
        if (!headPose) {
            return propagate(headPose.error());
        }
        parsed.headPose = *headPose;
    }
    // I10 step 4: the hands' controllers, posed like the head.
    for (const auto& [field, hand] : {std::pair{kLeftHandField, &parsed.leftHand}, std::pair{kRightHandField,
                                                                                             &parsed.rightHand}}) {
        if (const auto pose = step.find(field); pose != step.end()) {
            auto handPose = parseHeadPose(*pose, at.child(field));
            if (!handPose) {
                return propagate(handPose.error());
            }
            *hand = *handPose;
        }
    }
    // I10 step 5: the triggers.
    for (const auto& [field, select] : {std::pair{kLeftSelectField, &parsed.leftSelect},
                                        std::pair{kRightSelectField, &parsed.rightSelect}}) {
        if (const auto value = step.find(field); value != step.end()) {
            if (!value->is_number() || !std::isfinite(value->get<double>()) || value->get<double>() < 0.0 ||
                value->get<double>() > 1.0) {
                return fail(ErrorCode::ValidationFailed, "expected a number from 0 to 1", at.child(field));
            }
            *select = value->get<float>();
        }
    }

    const JsonLocation actionsAt = at.child(kActionsField);
    const auto actions = step.find(kActionsField);
    if (actions == step.end()) {
        if (!parsed.text.empty() || !parsed.phase.empty() || parsed.headPose || parsed.leftHand || parsed.rightHand ||
            parsed.leftSelect || parsed.rightSelect) {
            return parsed;
        }
        return fail(ErrorCode::ParseError,
                    "missing field \"actions\" (or \"text\", \"phase\", \"headPose\", \"leftHand\", \"rightHand\")",
                    actionsAt);
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

std::string InputScript::textAt(std::uint64_t frame) const {
    std::string text;
    for (const InputScriptStep& step : m_steps) {
        if (step.fromFrame == frame) {
            text += step.text;
        }
    }
    return text;
}

std::optional<ScriptedHeadPose> InputScript::headPoseAt(std::uint64_t frame) const {
    std::optional<ScriptedHeadPose> pose;
    for (const InputScriptStep& step : m_steps) {
        if (step.headPose && step.covers(frame)) {
            pose = step.headPose;
        }
    }
    return pose;
}

std::optional<ScriptedHeadPose> InputScript::handPoseAt(std::uint64_t frame, bool right) const {
    std::optional<ScriptedHeadPose> pose;
    for (const InputScriptStep& step : m_steps) {
        const std::optional<ScriptedHeadPose>& hand = right ? step.rightHand : step.leftHand;
        if (hand && step.covers(frame)) {
            pose = hand;
        }
    }
    return pose;
}

float InputScript::handSelectAt(std::uint64_t frame, bool right) const {
    float select = 0.0f;
    for (const InputScriptStep& step : m_steps) {
        const std::optional<float>& value = right ? step.rightSelect : step.leftSelect;
        if (value && step.covers(frame)) {
            select = *value;
        }
    }
    return select;
}

std::vector<std::string> InputScript::phasesAt(std::uint64_t frame) const {
    std::vector<std::string> phases;
    for (const InputScriptStep& step : m_steps) {
        if (!step.phase.empty() && step.covers(frame)) {
            phases.push_back(step.phase);
        }
    }
    std::sort(phases.begin(), phases.end());
    phases.erase(std::unique(phases.begin(), phases.end()), phases.end());
    return phases;
}

bool isValidPhaseName(std::string_view name) noexcept {
    if (name.empty() || name.size() > InputScript::kMaxPhaseNameBytes) {
        return false;
    }
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (!letter(name.front())) {
        return false;
    }
    return std::all_of(name.begin(), name.end(),
                       [&](char c) { return letter(c) || (c >= '0' && c <= '9') || c == '_' || c == '-'; });
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
