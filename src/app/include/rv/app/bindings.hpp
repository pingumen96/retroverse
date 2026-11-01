#pragma once

#include <rv/input/action_map.hpp>

namespace rv::core {
class Paths;
} // namespace rv::core

namespace rv::app {

/// The bindings of a run, layer by layer (ADR-0018 §2, §7).
struct LoadedBindings {
    /// What the user's hand-written file leaves: under settings.json.
    input::ActionMap below;
    /// In force: settings.json's section over `below` (unless left out), with no lock-out.
    input::ActionMap map;
    /// The shipped map: what the no-lock-out rule takes back.
    input::ActionMap shipped;
};

/// <asset root>/config/default_bindings.json (input::ActionMap::defaults() with a warning when it is missing or invalid),
/// then input::ActionMap::applyOverrides() with <user data>/config/bindings.json when that file exists (ignored with a
/// warning when invalid), then settings.json's section unless `safe` (--safe-bindings), then keepUnlocked(). Reads only;
/// never writes the user file.
[[nodiscard]] LoadedBindings loadBindingLayers(const core::Paths& paths, bool safe);
/// loadBindingLayers()'s map.
[[nodiscard]] input::ActionMap loadBindings(const core::Paths& paths, bool safe = false);

} // namespace rv::app
