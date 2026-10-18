#pragma once

// RoomInput (ADR-0010 §2): what the catalog gives the world for one room, the system's bulk snapshot
// (catalog::RoomSnapshot, ICatalogQuery::roomEntries) with the library it belongs to. world::selectRoom (I3 step 2)
// turns it into a RoomSelection, the generator's input. Header only.

#include <rv/catalog/ids.hpp>
#include <rv/catalog/room_entry.hpp>

namespace rv::world {

struct RoomInput {
    /// The library whose world the room belongs to: the world seed's default derives from it (world_config.hpp).
    catalog::LibraryId library;
    /// The system's entries and whether its media are known (RoomSnapshot::mediaKnown).
    catalog::RoomSnapshot snapshot;

    bool operator==(const RoomInput&) const = default;
};

} // namespace rv::world
