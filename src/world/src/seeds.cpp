#include <rv/world/seeds.hpp>

#include <string>

#include <rv/core/seed.hpp>

namespace rv::world {

std::uint64_t roomSeed(std::uint64_t worldSeed, const RoomId& room) {
    return core::deriveSeed(worldSeed, "room", room.toString());
}

std::uint64_t fixtureSeed(std::uint64_t roomSeed, const FixtureId& fixture) {
    return core::deriveSeed(roomSeed, "fixture", fixture.toString());
}

std::uint64_t propSeed(std::uint64_t roomSeed, const FixtureId& socketFixture) {
    return core::deriveSeed(roomSeed, "prop", socketFixture.localText());
}

std::uint64_t propSeed(std::uint64_t roomSeed, const LevelId& level) {
    return core::deriveSeed(roomSeed, "prop", level.localText());
}

std::uint64_t placementSeed(std::uint64_t roomSeed, const catalog::GameId& game, std::string_view mediaFamily) {
    return core::deriveSeed(roomSeed, "place", game.toString(), mediaFamily);
}

} // namespace rv::world
