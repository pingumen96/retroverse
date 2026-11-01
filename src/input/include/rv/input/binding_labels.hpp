#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <rv/core/expected.hpp>
#include <rv/input/action.hpp>
#include <rv/input/action_map.hpp>
#include <rv/input/binding.hpp>
#include <rv/input/codes.hpp>

namespace rv::input {

/// The keys' and buttons' names on screen (ADR-0018 §7): a string table, assets/ui/key_labels.json (Italian), the one
/// string table outside the RML documents: one name per key cannot be held by `data-if` lines. Pad buttons are text.
///
/// JSON format, schemaVersion 1:
///
///     {"schemaVersion": 1, "keys": {"enter": "Invio"}, "mouseButtons": {"left": "Clic"},
///      "padButtons": {"south": "A"}, "padAxes": {"left_trigger": "LT"},
///      "groups": {"up down left right": "Frecce"}}
///
/// Every map's keys are names of codes.hpp; "groups" gives one name to the codes listed together, space-separated in
/// the order a hint names them. Unknown fields or codes are errors; a code left out shows its own name in capitals.
class KeyLabels {
public:
    static constexpr int kSchemaVersion = 1;

    [[nodiscard]] static Expected<KeyLabels> fromJson(const nlohmann::json& document, std::string_view context);
    /// core::readJsonFile, then fromJson; the UTF-8 path is the error context.
    [[nodiscard]] static Expected<KeyLabels> loadFromFile(const std::filesystem::path& path);

    /// The binding's name: the table's, else the code's name in capitals.
    [[nodiscard]] std::string label(const Binding& binding) const;
    /// The name of `codes` (bindingCode()s joined by one space) as a group; empty when the table has none.
    [[nodiscard]] std::string group(std::string_view codes) const;

private:
    std::array<std::string, kKeyCount> m_keys;
    std::array<std::string, kMouseButtonCount> m_mouseButtons;
    std::array<std::string, kGamepadButtonCount> m_padButtons;
    std::array<std::string, kGamepadAxisCount> m_padAxes;
    std::vector<std::pair<std::string, std::string>> m_groups;
};

/// The stable name of the binding's code (codes.hpp): "enter", "south", "left_trigger".
[[nodiscard]] std::string_view bindingCode(const Binding& binding) noexcept;

/// True when the two bindings read the same key, button or axis, whatever their scales.
[[nodiscard]] bool sameInput(const Binding& left, const Binding& right) noexcept;

/// `action`'s binding a hint names for `device`: its first key (else its first mouse button), its first pad button
/// (else its first pad axis). Nothing when that device has none.
[[nodiscard]] std::optional<Binding> primaryBinding(const ActionMap& map, Action action, DeviceKind device);

/// primaryBinding()'s name in `labels`; empty when the device has none.
[[nodiscard]] std::string bindingLabel(const ActionMap& map, Action action, DeviceKind device,
                                       const KeyLabels& labels);

} // namespace rv::input
