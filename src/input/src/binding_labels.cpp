#include <rv/input/binding_labels.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <variant>

#include <nlohmann/json.hpp>

#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

#include "json_location.hpp"

namespace rv::input {
namespace {

using Json = nlohmann::json;
using detail::fail;
using detail::inQuotes;
using detail::JsonLocation;

/// A map of labels: every name must be a code `indexOf` knows.
template <std::size_t N, typename IndexOf>
Status readLabels(const Json& document, std::string_view field, std::array<std::string, N>& out, IndexOf indexOf,
                  const JsonLocation& root) {
    const auto found = document.find(field);
    if (found == document.end()) {
        return ok();
    }
    const JsonLocation at = root.child(field);
    if (!found->is_object()) {
        return fail(ErrorCode::ParseError, "expected an object mapping names to labels", at);
    }
    for (const auto& item : found->items()) {
        if (!item.value().is_string()) {
            return fail(ErrorCode::ParseError, "expected a label (a string)", at.child(item.key()));
        }
        const std::optional<std::size_t> index = indexOf(item.key());
        if (!index) {
            return fail(ErrorCode::ValidationFailed, "unknown name " + inQuotes(item.key()), at.child(item.key()));
        }
        out[*index] = item.value().get<std::string>();
    }
    return ok();
}

template <typename Code>
[[nodiscard]] std::optional<std::size_t> indexOfCode(std::optional<Code> code) noexcept {
    return code ? std::optional<std::size_t>(toIndex(*code)) : std::nullopt;
}

[[nodiscard]] std::string capitals(std::string_view name) {
    std::string text(name);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

struct CodeName {
    std::string_view operator()(const KeyBinding& binding) const noexcept { return keyName(binding.key); }
    std::string_view operator()(const MouseButtonBinding& binding) const noexcept {
        return mouseButtonName(binding.button);
    }
    std::string_view operator()(const MouseAxisBinding& binding) const noexcept { return mouseAxisName(binding.axis); }
    std::string_view operator()(const GamepadButtonBinding& binding) const noexcept {
        return gamepadButtonName(binding.button);
    }
    std::string_view operator()(const GamepadAxisBinding& binding) const noexcept {
        return gamepadAxisName(binding.axis);
    }
};

/// The first binding of `action` of the alternative `Kind`.
template <typename Kind>
[[nodiscard]] std::optional<Binding> firstOf(const ActionMap& map, Action action) {
    for (const Binding& binding : map.bindings(action)) {
        if (std::holds_alternative<Kind>(binding)) {
            return binding;
        }
    }
    return std::nullopt;
}

} // namespace

Expected<KeyLabels> KeyLabels::fromJson(const nlohmann::json& document, std::string_view context) {
    const JsonLocation root(context);
    if (!document.is_object()) {
        return fail(ErrorCode::ParseError, "expected a JSON object", root);
    }
    const auto version = document.find("schemaVersion");
    if (version == document.end() || !version->is_number_integer()) {
        return fail(ErrorCode::ParseError, "missing or wrong field \"schemaVersion\"", root.child("schemaVersion"));
    }
    if (*version != kSchemaVersion) {
        return fail(ErrorCode::Unsupported, "unsupported schemaVersion " + version->dump(),
                    root.child("schemaVersion"));
    }
    for (const auto& item : document.items()) {
        const std::string& field = item.key();
        if (field != "schemaVersion" && field != "keys" && field != "mouseButtons" && field != "padButtons" &&
            field != "padAxes" && field != "groups") {
            return fail(ErrorCode::ParseError, "unknown field " + inQuotes(field), root.child(field));
        }
    }
    KeyLabels labels;
    if (Status read = readLabels(
            document, "keys", labels.m_keys, [](const std::string& name) { return indexOfCode(keyFromName(name)); },
            root);
        !read) {
        return propagate(read.error());
    }
    if (Status read = readLabels(
            document, "mouseButtons", labels.m_mouseButtons,
            [](const std::string& name) { return indexOfCode(mouseButtonFromName(name)); }, root);
        !read) {
        return propagate(read.error());
    }
    if (Status read = readLabels(
            document, "padButtons", labels.m_padButtons,
            [](const std::string& name) { return indexOfCode(gamepadButtonFromName(name)); }, root);
        !read) {
        return propagate(read.error());
    }
    if (Status read = readLabels(
            document, "padAxes", labels.m_padAxes,
            [](const std::string& name) { return indexOfCode(gamepadAxisFromName(name)); }, root);
        !read) {
        return propagate(read.error());
    }
    if (const auto groups = document.find("groups"); groups != document.end()) {
        const JsonLocation at = root.child("groups");
        if (!groups->is_object()) {
            return fail(ErrorCode::ParseError, "expected an object mapping codes to labels", at);
        }
        for (const auto& item : groups->items()) {
            if (!item.value().is_string()) {
                return fail(ErrorCode::ParseError, "expected a label (a string)", at.child(item.key()));
            }
            labels.m_groups.emplace_back(item.key(), item.value().get<std::string>());
        }
    }
    return labels;
}

Expected<KeyLabels> KeyLabels::loadFromFile(const std::filesystem::path& path) {
    const std::string context = core::pathToUtf8(path);
    try {
        const auto document = core::readJsonFile(path);
        if (!document) {
            return propagate(document.error());
        }
        return fromJson(*document, context);
    } catch (const Json::exception& e) {
        return makeError(ErrorCode::ParseError, e.what(), context);
    }
}

std::string KeyLabels::label(const Binding& binding) const {
    const std::string* held = nullptr;
    if (const auto* key = std::get_if<KeyBinding>(&binding); key != nullptr && toIndex(key->key) < kKeyCount) {
        held = &m_keys[toIndex(key->key)];
    } else if (const auto* mouse = std::get_if<MouseButtonBinding>(&binding);
               mouse != nullptr && toIndex(mouse->button) < kMouseButtonCount) {
        held = &m_mouseButtons[toIndex(mouse->button)];
    } else if (const auto* button = std::get_if<GamepadButtonBinding>(&binding);
               button != nullptr && toIndex(button->button) < kGamepadButtonCount) {
        held = &m_padButtons[toIndex(button->button)];
    } else if (const auto* axis = std::get_if<GamepadAxisBinding>(&binding);
               axis != nullptr && toIndex(axis->axis) < kGamepadAxisCount) {
        held = &m_padAxes[toIndex(axis->axis)];
    }
    return held != nullptr && !held->empty() ? *held : capitals(bindingCode(binding));
}

std::string KeyLabels::group(std::string_view codes) const {
    const auto found =
        std::find_if(m_groups.begin(), m_groups.end(), [&](const auto& entry) { return entry.first == codes; });
    return found != m_groups.end() ? found->second : std::string();
}

std::string_view bindingCode(const Binding& binding) noexcept {
    return std::visit(CodeName{}, binding);
}

bool sameInput(const Binding& left, const Binding& right) noexcept {
    return left.index() == right.index() && bindingCode(left) == bindingCode(right);
}

std::optional<Binding> primaryBinding(const ActionMap& map, Action action, DeviceKind device) {
    if (device == DeviceKind::Gamepad) {
        if (auto button = firstOf<GamepadButtonBinding>(map, action)) {
            return button;
        }
        return firstOf<GamepadAxisBinding>(map, action);
    }
    if (auto key = firstOf<KeyBinding>(map, action)) {
        return key;
    }
    return firstOf<MouseButtonBinding>(map, action);
}

std::string bindingLabel(const ActionMap& map, Action action, DeviceKind device, const KeyLabels& labels) {
    const std::optional<Binding> binding = primaryBinding(map, action, device);
    return binding ? labels.label(*binding) : std::string();
}

} // namespace rv::input
