#pragma once

// The local seeds of style §13.3 on core::deriveSeed (ADR-0010 §7; I3 step 3), so that one decision of the generator
// never shifts another's random sequence:
//   roomSeed      = deriveSeed(worldSeed, "room", "room:snes")
//   fixtureSeed   = deriveSeed(roomSeed, "fixture", "room:snes/fx:w03")    the fixture's id (ADR-0010 §7)
//   propSeed      = deriveSeed(roomSeed, "prop", "fx:dec-sw")               a socket's decoration
//                 = deriveSeed(roomSeed, "prop", "fx:w03/l:2")              a dressed level, ADR-0010's socket of a level
//   placementSeed = deriveSeed(roomSeed, "place", "g0123456789abcdef", "cartridge_home_16bit")
// as the oracle's chain (scripts/world_seed_oracle.py) spells them. A parent seed enters as its decimal digits; ids
// enter as their text, already validated (no NUL, UTF-8); a prop's socket is named within its room
// (FixtureId::localText(), LevelId::localText()).

#include <cstdint>
#include <string_view>

#include <rv/catalog/ids.hpp>
#include <rv/world/ids.hpp>

namespace rv::world {

[[nodiscard]] std::uint64_t roomSeed(std::uint64_t worldSeed, const RoomId& room);
/// A fixture's own variations (materials, wear: the scene's, I3 step 10a and later); the generator draws nothing from
/// it in I3.
[[nodiscard]] std::uint64_t fixtureSeed(std::uint64_t roomSeed, const FixtureId& fixture);
/// The decoration a socket holds (which choice, whether a decoration socket is used).
[[nodiscard]] std::uint64_t propSeed(std::uint64_t roomSeed, const FixtureId& socketFixture);
/// The shelf dressing of a level.
[[nodiscard]] std::uint64_t propSeed(std::uint64_t roomSeed, const LevelId& level);
/// A placement's offset (ADR-0010 §6), by the game and the media family it stands as.
[[nodiscard]] std::uint64_t placementSeed(std::uint64_t roomSeed, const catalog::GameId& game,
                                          std::string_view mediaFamily);

} // namespace rv::world
