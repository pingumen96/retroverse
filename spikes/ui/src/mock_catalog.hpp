#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include <rv/ui/catalog_source.hpp>

namespace rv::spike_ui {

/// The size of the mock catalog: the scale of the reference collection (about 90,000 games once disc tracks are
/// grouped, ADR-0008), rounded up.
inline constexpr std::size_t kMockCatalogSize = 100000;

/// The developer of the marker game, a word no other game has: a search for it finds the marker alone (the scenario's
/// way to the middle of the list).
inline constexpr std::string_view kMarkerWord = "mezzanotte";

/// `count` generated games (at least 1): invented Italian titles, studios and descriptions (no trademark, style §17.2)
/// over twelve generic platform families, years 1977 to 2006, a few games that cannot start (CatalogEntry::state), ids
/// in the GameId text form ('g' and 16 lowercase hexadecimal digits of a hash of the index: unordered, as real GameIds
/// are). Sorted as the catalog sorts (the title folded to lowercase, then the id); the game in the middle of the list
/// (count / 2) is the marker: its developer is "Studio Mezzanotte" and its platform the first game's (the first
/// platform chip). Deterministic: the same games in every run.
[[nodiscard]] std::vector<ui::CatalogEntry> mockCatalog(std::size_t count = kMockCatalogSize);

} // namespace rv::spike_ui
