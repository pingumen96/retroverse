#pragma once

#include <rv/input/action_map.hpp>

namespace rv::core {
class Paths;
} // namespace rv::core

namespace rv::app {

/// Bindings of an application: <asset root>/config/default_bindings.json (input::ActionMap::defaults() with a warning
/// when it is missing or invalid), then input::ActionMap::applyOverrides() with <user data>/config/bindings.json when
/// that file exists (ignored with a warning when invalid). Reads only; never writes the user file.
[[nodiscard]] input::ActionMap loadBindings(const core::Paths& paths);

} // namespace rv::app
