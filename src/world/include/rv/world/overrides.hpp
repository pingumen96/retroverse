#pragma once

// overrides.json (ADR-0010 §9, §10; I3 step 9): <user data>/worlds/<library id>/overrides.json holds the world's pins,
// a game held in a slot and a fixture held in its socket (with its prefab, when the pin names one). Retroverse reads it
// and never writes it: I4's editor will (until then a test, or the user by hand). A missing file holds no pin.
//
// The file (JSON with comments, every key but schemaVersion optional):
//   {"schemaVersion": 1,
//    "gamePins": [{"game": "g0123456789abcdef", "slot": "room:snes/fx:w01/s:3.12"}],
//    "fixturePins": [{"fixture": "room:snes/fx:w05", "prefab": {"manifest": "decor.poster.generic",
//                                                                 "kind": "poster_frame"}}]}
// A game is pinned once and a slot holds one pin; a fixture is pinned once; "prefab" is optional. A file of a newer
// schemaVersion is refused (written by a newer Retroverse).
//
// In I3 pins are protected and reported, never applied: the generator and the incremental update never move or remove
// a pinned object (a change that would is not applied and is reported as needing a confirmation, which I4's editor
// asks for), a pin that names a game, slot, fixture or socket that no longer exists is reported as orphaned, and a pin
// the plan does not honour is reported (ADR-0010 §10: "Overrides are applied last ... A change that would move a pinned
// object is not applied and is reported").

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/ids.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/ids.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::world {

/// overrides.json's format.
inline constexpr std::int64_t kOverridesSchemaVersion = 1;

/// A game held in a slot.
struct GamePin {
    catalog::GameId game;
    SlotId slot;

    bool operator==(const GamePin&) const = default;
};

/// A fixture held in its socket, with its prefab when the pin names one.
struct FixturePin {
    FixtureId fixture;
    std::optional<PrefabRef> prefab;

    bool operator==(const FixturePin&) const = default;
};

/// The world's overrides: its pins, games by GameId and fixtures by id.
struct WorldOverrides {
    std::vector<GamePin> gamePins;
    std::vector<FixturePin> fixturePins;

    [[nodiscard]] bool empty() const noexcept { return gamePins.empty() && fixturePins.empty(); }

    bool operator==(const WorldOverrides&) const = default;
};

/// Reads overrides.json's text. Errors (context and line): ParseError, ValidationFailed (a key unknown, missing or of
/// another type, a malformed GameId, slot or fixture id, a game, slot or fixture pinned twice), Unsupported (a newer
/// schemaVersion).
[[nodiscard]] Expected<WorldOverrides> parseOverrides(std::string_view text, std::string_view context);
/// Reads the file; a missing file holds no pin. Errors: those of reading the file (other than NotFound) and of
/// parseOverrides().
[[nodiscard]] Expected<WorldOverrides> loadOverrides(const std::filesystem::path& file);

/// The pins measured against a plan.
struct PinCheck {
    /// The pins that name a game, slot, fixture or socket the plan or its room's template no longer holds: never moved,
    /// reported (ChangeReport::orphanedPins), each as pinText().
    std::vector<std::string> orphaned;
    /// The pins the plan does not honour although what they name is there: applying them would move or replace
    /// something, which I3 never does (ChangeReport::needsConfirmation), each as pinText().
    std::vector<std::string> unhonoured;
    /// The pins the plan honours, and the orphaned ones.
    WorldOverrides honouredOrOrphaned;
};

/// Measures `overrides` against `plan` (its rooms' templates in `templates`, by id): a game pin is honoured when its
/// game stands in its slot, orphaned when the slot is not one of the plan's (fixture, level and index) or the game has
/// no entry in the plan; a fixture pin is honoured when the plan holds the fixture (with the pin's prefab, when it
/// names one), orphaned when its room is not the plan's or its socket is not in the room's template. Lists sorted.
[[nodiscard]] PinCheck checkPins(const WorldPlan& plan, const std::vector<RoomTemplate>& templates,
                                 const WorldOverrides& overrides);

/// "game g0123456789abcdef pinned to room:snes/fx:w01/s:3.12", "fixture room:snes/fx:w05 pinned" (with " as
/// <manifest>" when the pin names a prefab).
[[nodiscard]] std::string pinText(const GamePin& pin);
[[nodiscard]] std::string pinText(const FixturePin& pin);

} // namespace rv::world
