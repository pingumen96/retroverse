#include <rv/input/action_map.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

namespace rv::input {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kSchemaVersionField = "schemaVersion";
constexpr std::string_view kBindingsField = "bindings";

// Device fields of a binding entry, one per Binding alternative.
constexpr std::string_view kKeyField = "key";
constexpr std::string_view kMouseButtonField = "mouseButton";
constexpr std::string_view kMouseAxisField = "mouseAxis";
constexpr std::string_view kPadButtonField = "padButton";
constexpr std::string_view kPadAxisField = "padAxis";
constexpr std::array<std::string_view, 5> kDeviceFields{kKeyField, kMouseButtonField, kMouseAxisField,
                                                        kPadButtonField, kPadAxisField};

constexpr std::string_view kScaleField = "scale";
constexpr std::string_view kDeadzoneField = "deadzone";
constexpr std::string_view kExponentField = "exponent";

/// Location of a JSON value for error contexts: "<document>:<JSON pointer>" (RFC 6901 escaping).
class JsonLocation {
public:
    explicit JsonLocation(std::string_view document) : m_document(document) {}

    [[nodiscard]] JsonLocation child(std::string_view token) const {
        JsonLocation result = *this;
        result.m_pointer += '/';
        for (const char c : token) {
            if (c == '~') {
                result.m_pointer += "~0";
            } else if (c == '/') {
                result.m_pointer += "~1";
            } else {
                result.m_pointer += c;
            }
        }
        return result;
    }

    [[nodiscard]] JsonLocation child(std::size_t index) const { return child(std::to_string(index)); }

    [[nodiscard]] std::string str() const {
        if (m_pointer.empty()) {
            return std::string(m_document);
        }
        if (m_document.empty()) {
            return m_pointer;
        }
        return std::string(m_document) + ":" + m_pointer;
    }

private:
    std::string_view m_document;
    std::string m_pointer;
};

[[nodiscard]] tl::unexpected<Error> fail(ErrorCode code, std::string message, const JsonLocation& at) {
    return makeError(code, std::move(message), at.str());
}

[[nodiscard]] std::string inQuotes(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

/// Optional numeric field read as a finite float; `fallback` when the field is absent.
Expected<float> readOptionalNumber(const Json& object, std::string_view field, float fallback,
                                   const JsonLocation& at) {
    const auto it = object.find(field);
    if (it == object.end()) {
        return fallback;
    }
    if (!it->is_number()) {
        return fail(ErrorCode::ParseError, "expected a number", at.child(field));
    }
    const double value = it->get<double>();
    if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        return fail(ErrorCode::ValidationFailed, "number out of range", at.child(field));
    }
    return static_cast<float>(value);
}

Expected<Binding> parseBinding(const Json& entry, const JsonLocation& at) {
    if (!entry.is_object()) {
        return fail(ErrorCode::ParseError, "expected a binding object such as {\"key\": \"w\"}", at);
    }

    // Unknown fields first, so a typo such as "kee" is reported as such rather than as a missing device field.
    std::string_view device;
    for (const auto& item : entry.items()) {
        const std::string& field = item.key();
        const auto deviceField = std::find(kDeviceFields.begin(), kDeviceFields.end(), field);
        if (deviceField == kDeviceFields.end()) {
            if (field != kScaleField && field != kDeadzoneField && field != kExponentField) {
                return fail(ErrorCode::ParseError, "unknown field " + inQuotes(field), at.child(field));
            }
            continue;
        }
        if (!device.empty()) {
            return fail(ErrorCode::ParseError,
                        "binding has two device fields, " + inQuotes(device) + " and " + inQuotes(field) +
                            "; exactly one is allowed",
                        at);
        }
        device = *deviceField;
    }
    if (device.empty()) {
        return fail(ErrorCode::ParseError,
                    "binding needs one device field: \"key\", \"mouseButton\", \"mouseAxis\", \"padButton\" or "
                    "\"padAxis\"",
                    at);
    }
    if (device != kPadAxisField) {
        for (const std::string_view field : {kDeadzoneField, kExponentField}) {
            if (entry.contains(field)) {
                return fail(ErrorCode::ParseError, inQuotes(field) + " is only allowed in \"padAxis\" bindings",
                            at.child(field));
            }
        }
    }

    const JsonLocation codeAt = at.child(device);
    const Json& code = *entry.find(device);
    if (!code.is_string()) {
        return fail(ErrorCode::ParseError, "expected a name string", codeAt);
    }
    const std::string& name = code.get_ref<const std::string&>();

    const auto scale = readOptionalNumber(entry, kScaleField, 1.0f, at);
    if (!scale) {
        return propagate(scale.error());
    }

    const auto unknown = [&](std::string_view what) {
        return fail(ErrorCode::ValidationFailed, "unknown " + std::string(what) + " " + inQuotes(name), codeAt);
    };

    if (device == kKeyField) {
        const auto key = keyFromName(name);
        if (!key) {
            return fail(ErrorCode::ValidationFailed,
                        "unknown key " + inQuotes(name) + " (names are lowercase: \"w\", \"left_shift\", \"f1\", ...)",
                        codeAt);
        }
        return Binding{KeyBinding{*key, *scale}};
    }
    if (device == kMouseButtonField) {
        const auto button = mouseButtonFromName(name);
        if (!button) {
            return unknown("mouse button");
        }
        return Binding{MouseButtonBinding{*button, *scale}};
    }
    if (device == kMouseAxisField) {
        const auto axis = mouseAxisFromName(name);
        if (!axis) {
            return unknown("mouse axis");
        }
        return Binding{MouseAxisBinding{*axis, *scale}};
    }
    if (device == kPadButtonField) {
        const auto button = gamepadButtonFromName(name);
        if (!button) {
            return unknown("gamepad button");
        }
        return Binding{GamepadButtonBinding{*button, *scale}};
    }

    const auto axis = gamepadAxisFromName(name);
    if (!axis) {
        return unknown("gamepad axis");
    }
    const auto deadzone = readOptionalNumber(entry, kDeadzoneField, kDefaultDeadzone, at);
    if (!deadzone) {
        return propagate(deadzone.error());
    }
    if (!(*deadzone >= 0.0f && *deadzone < 1.0f)) {
        return fail(ErrorCode::ValidationFailed, "deadzone must be in [0, 1)", at.child(kDeadzoneField));
    }
    const auto exponent = readOptionalNumber(entry, kExponentField, kDefaultExponent, at);
    if (!exponent) {
        return propagate(exponent.error());
    }
    if (!(*exponent > 0.0f)) {
        return fail(ErrorCode::ValidationFailed, "exponent must be greater than 0", at.child(kExponentField));
    }
    return Binding{GamepadAxisBinding{*axis, *scale, *deadzone, *exponent}};
}

/// Shortest decimal form that reads back as the same float, so written files show 0.15 instead of the exact
/// binary value 0.15000000596046448.
[[nodiscard]] Json toJsonNumber(float value) {
    std::array<char, 32> text{};
    double shortest = static_cast<double>(value);
    const auto written = std::to_chars(text.data(), text.data() + text.size(), value);
    if (written.ec == std::errc{}) {
        // Leaves `shortest` untouched on failure.
        static_cast<void>(std::from_chars(text.data(), written.ptr, shortest));
    }
    return shortest;
}

[[nodiscard]] Json entryToJson(std::string_view deviceField, std::string_view code, float scale) {
    Json entry = Json::object();
    entry[deviceField] = code;
    entry[kScaleField] = toJsonNumber(scale);
    return entry;
}

struct BindingWriter {
    Json operator()(const KeyBinding& binding) const {
        return entryToJson(kKeyField, keyName(binding.key), binding.scale);
    }
    Json operator()(const MouseButtonBinding& binding) const {
        return entryToJson(kMouseButtonField, mouseButtonName(binding.button), binding.scale);
    }
    Json operator()(const MouseAxisBinding& binding) const {
        return entryToJson(kMouseAxisField, mouseAxisName(binding.axis), binding.scale);
    }
    Json operator()(const GamepadButtonBinding& binding) const {
        return entryToJson(kPadButtonField, gamepadButtonName(binding.button), binding.scale);
    }
    Json operator()(const GamepadAxisBinding& binding) const {
        Json entry = entryToJson(kPadAxisField, gamepadAxisName(binding.axis), binding.scale);
        entry[kDeadzoneField] = toJsonNumber(binding.deadzone);
        entry[kExponentField] = toJsonNumber(binding.exponent);
        return entry;
    }
};

} // namespace

void ActionMap::bind(Action action, Binding binding) {
    const std::size_t index = toIndex(action);
    if (index >= kActionCount || !isValid(binding)) {
        return;
    }
    std::vector<Binding>& list = m_bindings[index];
    if (list.size() < kMaxBindingsPerAction && std::find(list.begin(), list.end(), binding) == list.end()) {
        list.push_back(binding);
    }
}

void ActionMap::clear(Action action) {
    const std::size_t index = toIndex(action);
    if (index < kActionCount) {
        m_bindings[index].clear();
    }
}

std::span<const Binding> ActionMap::bindings(Action action) const noexcept {
    const std::size_t index = toIndex(action);
    if (index >= kActionCount) {
        return {};
    }
    return m_bindings[index];
}

ActionMap ActionMap::defaults() {
    ActionMap map;

    map.bind(Action::MoveForward, KeyBinding{Key::W, 1.0f});
    map.bind(Action::MoveForward, KeyBinding{Key::S, -1.0f});
    map.bind(Action::MoveForward, KeyBinding{Key::Up, 1.0f});
    map.bind(Action::MoveForward, KeyBinding{Key::Down, -1.0f});
    map.bind(Action::MoveForward, GamepadAxisBinding{GamepadAxis::LeftY, 1.0f});

    map.bind(Action::MoveRight, KeyBinding{Key::D, 1.0f});
    map.bind(Action::MoveRight, KeyBinding{Key::A, -1.0f});
    map.bind(Action::MoveRight, KeyBinding{Key::Right, 1.0f});
    map.bind(Action::MoveRight, KeyBinding{Key::Left, -1.0f});
    map.bind(Action::MoveRight, GamepadAxisBinding{GamepadAxis::LeftX, 1.0f});

    map.bind(Action::LookX, GamepadAxisBinding{GamepadAxis::RightX, 1.0f});
    map.bind(Action::LookY, GamepadAxisBinding{GamepadAxis::RightY, 1.0f});
    map.bind(Action::LookDeltaX, MouseAxisBinding{MouseAxis::DeltaX, 1.0f});
    // Mouse +Y is down on screen: moving the mouse up must look up.
    map.bind(Action::LookDeltaY, MouseAxisBinding{MouseAxis::DeltaY, -1.0f});

    map.bind(Action::Sprint, KeyBinding{Key::LeftShift, 1.0f});
    map.bind(Action::Sprint, GamepadButtonBinding{GamepadButton::LeftStick, 1.0f});

    map.bind(Action::Interact, KeyBinding{Key::E, 1.0f});
    map.bind(Action::Interact, MouseButtonBinding{MouseButton::Left, 1.0f});
    map.bind(Action::Interact, GamepadButtonBinding{GamepadButton::South, 1.0f});

    map.bind(Action::Back, KeyBinding{Key::Escape, 1.0f});
    map.bind(Action::Back, GamepadButtonBinding{GamepadButton::East, 1.0f});

    map.bind(Action::QuickCatalog, KeyBinding{Key::Tab, 1.0f});
    map.bind(Action::QuickCatalog, GamepadButtonBinding{GamepadButton::North, 1.0f});

    map.bind(Action::Confirm, KeyBinding{Key::Enter, 1.0f});
    map.bind(Action::Confirm, GamepadButtonBinding{GamepadButton::Start, 1.0f});

    map.bind(Action::Map, KeyBinding{Key::M, 1.0f});
    map.bind(Action::Map, GamepadButtonBinding{GamepadButton::Back, 1.0f});

    map.bind(Action::Menu, GamepadButtonBinding{GamepadButton::Start, 1.0f});

    map.bind(Action::ToggleDevUI, KeyBinding{Key::F1, 1.0f});
    map.bind(Action::ReloadShaders, KeyBinding{Key::F5, 1.0f});
    map.bind(Action::Screenshot, KeyBinding{Key::F12, 1.0f});

    return map;
}

Expected<ActionMap> ActionMap::fromJson(const nlohmann::json& json, std::string_view context) {
    const JsonLocation root(context);
    if (!json.is_object()) {
        return fail(ErrorCode::ParseError, "expected a JSON object with \"schemaVersion\" and \"bindings\"", root);
    }
    for (const auto& item : json.items()) {
        if (item.key() != kSchemaVersionField && item.key() != kBindingsField) {
            return fail(ErrorCode::ParseError, "unknown field " + inQuotes(item.key()), root.child(item.key()));
        }
    }

    const JsonLocation versionAt = root.child(kSchemaVersionField);
    const auto version = json.find(kSchemaVersionField);
    if (version == json.end()) {
        return fail(ErrorCode::ParseError, "missing field \"schemaVersion\"", versionAt);
    }
    if (!version->is_number_integer()) {
        return fail(ErrorCode::ParseError, "expected an integer", versionAt);
    }
    if (*version != kSchemaVersion) {
        return fail(ErrorCode::Unsupported,
                    "unsupported schemaVersion " + version->dump() + " (expected " + std::to_string(kSchemaVersion) +
                        ")",
                    versionAt);
    }

    const JsonLocation bindingsAt = root.child(kBindingsField);
    const auto bindings = json.find(kBindingsField);
    if (bindings == json.end()) {
        return fail(ErrorCode::ParseError, "missing field \"bindings\"", bindingsAt);
    }
    if (!bindings->is_object()) {
        return fail(ErrorCode::ParseError, "expected an object mapping action names to binding arrays", bindingsAt);
    }

    ActionMap map;
    for (const auto& item : bindings->items()) {
        const JsonLocation actionAt = bindingsAt.child(item.key());
        const auto action = actionFromName(item.key());
        if (!action) {
            return fail(ErrorCode::ValidationFailed, "unknown action " + inQuotes(item.key()), actionAt);
        }
        const Json& list = item.value();
        if (!list.is_array()) {
            return fail(ErrorCode::ParseError, "expected an array of bindings", actionAt);
        }
        if (list.size() > kMaxBindingsPerAction) {
            return fail(ErrorCode::ValidationFailed,
                        "too many bindings: " + std::to_string(list.size()) + " (at most " +
                            std::to_string(kMaxBindingsPerAction) + ")",
                        actionAt);
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            auto binding = parseBinding(list[i], actionAt.child(i));
            if (!binding) {
                return propagate(std::move(binding.error()));
            }
            map.bind(*action, *binding);
        }
    }
    return map;
}

nlohmann::json ActionMap::toJson() const {
    Json bindings = Json::object();
    for (std::size_t index = 0; index < kActionCount; ++index) {
        Json list = Json::array();
        for (const Binding& binding : m_bindings[index]) {
            list.push_back(std::visit(BindingWriter{}, binding));
        }
        bindings[actionName(static_cast<Action>(index))] = std::move(list);
    }
    Json document = Json::object();
    document[kSchemaVersionField] = kSchemaVersion;
    document[kBindingsField] = std::move(bindings);
    return document;
}

Expected<ActionMap> ActionMap::loadFromFile(const std::filesystem::path& path) {
    const auto document = core::readJsonFile(path);
    if (!document) {
        return propagate(document.error());
    }
    const std::string context = core::pathToUtf8(path);
    return fromJson(*document, context);
}

} // namespace rv::input
