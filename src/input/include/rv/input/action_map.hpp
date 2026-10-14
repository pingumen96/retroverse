#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/expected.hpp>
#include <rv/input/action.hpp>
#include <rv/input/binding.hpp>

namespace rv::input {

/// The bindings of every Action: which keys, buttons and axes drive it. Small value type (a few dozen bindings),
/// cheap to copy. Built in code (defaults()) or loaded from a JSON binding file.
///
/// JSON format, schemaVersion 1 (assets/config/default_bindings.json):
///
///     {
///       "schemaVersion": 1,
///       "bindings": {
///         "move_forward": [ {"key": "w", "scale": 1}, {"key": "s", "scale": -1},
///                           {"padAxis": "left_y", "scale": 1, "deadzone": 0.15, "exponent": 1} ],
///         "interact": [ {"key": "e"}, {"mouseButton": "left"}, {"padButton": "south"} ]
///       }
///     }
///
/// Binding lists are keyed by actionName(). Each entry has exactly one device field, "key", "mouseButton",
/// "mouseAxis", "padButton" or "padAxis", whose value is a name from codes.hpp; an optional "scale" (default 1);
/// and, for "padAxis" entries only, optional "deadzone" (default 0.15, in [0, 1)) and "exponent" (default 1,
/// > 0). Actions missing from the document have no bindings; exact duplicates collapse into one binding. Unknown
/// fields and lists longer than kMaxBindingsPerAction are errors.
class ActionMap {
public:
    static constexpr int kSchemaVersion = 1;
    /// Bindings per action. Real maps use a handful; the cap keeps a hostile file from making loading quadratic.
    static constexpr std::size_t kMaxBindingsPerAction = 32;

    /// Appends `binding` to the bindings of `action`. Ignored when the action is out of range, the binding is
    /// invalid (see isValid()), an identical binding is already present (binding twice never doubles a
    /// contribution) or the action already has kMaxBindingsPerAction bindings.
    void bind(Action action, Binding binding);
    /// Removes every binding of `action`.
    void clear(Action action);
    /// Bindings of `action` in insertion order; empty for an out-of-range action. The span is invalidated by
    /// bind() and clear().
    [[nodiscard]] std::span<const Binding> bindings(Action action) const noexcept;

    /// Default controls of specification §12.1; equal to fromJson() of assets/config/default_bindings.json.
    [[nodiscard]] static ActionMap defaults();

    /// Parses a binding document. Error contexts are "<context>:<JSON pointer>" (RFC 6901), for example
    /// "default_bindings.json:/bindings/move_forward/1/key"; just "<context>" when the document root is at fault.
    /// Error codes: ParseError (wrong JSON type, missing or unknown field, zero or several device fields),
    /// ValidationFailed (unknown action or code name, number not finite once rounded to float, too many
    /// bindings), Unsupported (schemaVersion other than kSchemaVersion, checked before any other field).
    [[nodiscard]] static Expected<ActionMap> fromJson(const nlohmann::json& json, std::string_view context);

    /// User overrides on top of this map: every action listed in the binding document (same format, validation and
    /// errors as fromJson) gets the document's bindings, an empty list unbinding it; the actions the document leaves
    /// out keep their current bindings, so a user file only names what it changes. Returns how many actions were
    /// replaced. On error the map is unchanged.
    [[nodiscard]] Expected<std::size_t> applyOverrides(const nlohmann::json& document, std::string_view context);

    /// Serializes every action, unbound ones as empty arrays, with every field explicit and numbers in their
    /// shortest exact form (0.15, not 0.15000000596046448). fromJson(toJson()) reproduces the map exactly.
    [[nodiscard]] nlohmann::json toJson() const;

    /// Reads a binding file (rv::core::readJsonFile, comments allowed) and parses it with the UTF-8 file path as
    /// the error context. Never throws on bad content: malformed JSON, including a number that overflows a
    /// double, is a ParseError.
    [[nodiscard]] static Expected<ActionMap> loadFromFile(const std::filesystem::path& path);

    /// Same bindings in the same order for every action.
    bool operator==(const ActionMap&) const = default;

private:
    std::array<std::vector<Binding>, kActionCount> m_bindings;
};

} // namespace rv::input
