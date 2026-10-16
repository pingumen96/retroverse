#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::launcher {

/// One variable of a process environment (UTF-8).
struct EnvironmentVariable {
    std::string name;
    std::string value;

    friend bool operator==(const EnvironmentVariable&, const EnvironmentVariable&) = default;
};

/// A change a launch request makes to the environment it inherits: sets `name` to `value`, or removes the variable
/// when `value` is empty (std::nullopt; an empty string sets an empty value).
struct EnvironmentOverride {
    std::string name;
    std::optional<std::string> value;

    friend bool operator==(const EnvironmentOverride&, const EnvironmentOverride&) = default;
};

/// Windows compares environment variable names without regard to case: ASCII letters are compared case-insensitively,
/// other characters exactly.
[[nodiscard]] bool sameVariableName(std::string_view a, std::string_view b) noexcept;

/// A name a request may set or remove: not empty, without '=' (it separates name and value; the hidden per-drive
/// variables such as "=C:" are not the launcher's business), without NUL, valid UTF-8. The value must hold no NUL and
/// be valid UTF-8. Errors are ValidationFailed with the variable's name as context.
[[nodiscard]] Status validateOverride(const EnvironmentOverride& override);

/// The environment of a started process: `inherited` (Retroverse's own, in any order) with `overrides` applied in order
/// (a later override of the same name wins; names compared by sameVariableName, the existing spelling of a name kept
/// when its value is replaced), sorted by name case-insensitively as CreateProcessW's documentation asks of an
/// environment block. Inherited entries whose name starts with '=' (per-drive current directories) are kept as they
/// are.
[[nodiscard]] std::vector<EnvironmentVariable> mergeEnvironment(std::span<const EnvironmentVariable> inherited,
                                                                std::span<const EnvironmentOverride> overrides);

} // namespace rv::launcher
