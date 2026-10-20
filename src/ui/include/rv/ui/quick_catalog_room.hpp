#pragma once

// GPU-free (part of rv_ui_model, header only): "Mostra nella stanza" on the quick catalog's card (I3 step 15a,
// ADR-0012 §8), the keys the document (assets/ui/quick_catalog.rml) and its screen (rv::ui::QuickCatalogScreen) share
// with the application, which knows the room (rv_app's roomPlacementOf()).

#include <array>
#include <string_view>

namespace rv::ui::quick_catalog {

/// The card's button "Mostra nella stanza" (in the card's actions region, below Avvia) and its data event.
inline constexpr std::string_view kShowInRoomId = "act-room";
inline constexpr std::string_view kShowInRoomEvent = "show_in_room";
/// The card's room key (the data model's card.room): empty while no room is shown (the button and the sentence absent),
/// kRoomPlaced for a game the room's plan places (the button shows), else why it does not stand in the room: a
/// sentence of the document each.
inline constexpr std::string_view kRoomPlaced = "placed";
/// The plan's unplaced reasons (rv_world's UnplacedReason names, world::toString), and kRoomNotInRoom: a game the plan
/// does not hold at all (another system's: the room collects one system). tests/unit/app checks that the world's names
/// are these.
inline constexpr std::string_view kRoomNotInRoom = "not-in-room";
inline constexpr std::array<std::string_view, 6> kRoomUnplacedKeys{"duplicate-title", "no-cover", "unavailable",
                                                                   "capacity",        "room-kept", kRoomNotInRoom};

} // namespace rv::ui::quick_catalog
