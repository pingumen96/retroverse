#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// The hub's favourites showcase (I6 step 5, ADR-0014 §3, ADR-0013 §10): a display case in the ground floor's hub whose
// cases are runtime proxies of the catalog's favourite and recently played games, never plan placements. Its slots
// are the hub's display case's (world::hubShowcaseSlots(), moved into the scene's frame); its games the catalog's
// favoritesAndRecent (the directory's list, step 7a/7b) in its order, at most one per slot; each drawn as a face-out
// case of kShowcaseFamily (one family for every proxy: a keep case, whatever the game's own room shows), its cover
// from the rooms' items files (the game's system's: a game no room places, or no file knows, shows its generated
// label). The app (WorldSession, rv_app) draws them as a collection of its own (registry keys under
// kShowcaseKeyPrefix, one collection per floor shown, updated in place by a new content), brings their layers in with a cover loader of their own, picks them with a
// pick index of their own, and on interact opens the directory at the game's card (Avvia, Mostra nella stanza). The
// content is made again when a launched game returns and after a refresh of the catalog; a content equal to the one
// shown is not drawn again.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <rv/app/collection_layers.hpp>
#include <rv/app/room_items.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog/recent_games.hpp>
#include <rv/core/expected.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/scene_builder.hpp>

namespace rv::app {

struct WorldCatalogReader;

/// The proxies' media family (a keep case: a cover's portrait shape) and their collection's registry key prefix.
inline constexpr std::string_view kShowcaseFamily = "disc_case_dvd_v2";
inline constexpr std::string_view kShowcaseKeyPrefix = "collection/hub:showcase";

/// The showcase of a floor build: its slots in the scene's frame and the proxies' scene data (their family and its item
/// prefab).
struct HubShowcase {
    std::vector<glm::mat4> slots;
    glm::vec3 slotSize{0.0f};
    world::SceneData data;
};

/// A game of the showcase: the catalog's favourite or recent game and what its cover needs (from its system's items
/// file: its cover's path, its file's title, its year; empty when no file knows it).
struct ShowcaseGame {
    catalog::GameId game;
    std::string title;
    catalog::SystemName system;
    std::string systemName;
    bool favorite = false;
    std::string coverPath;
    std::string fallbackTitle;
    std::string year;

    bool operator==(const ShowcaseGame&) const = default;
};

/// The proxies' scene data: kShowcaseFamily's registry entry and its item prefab (resolved as rooms resolve theirs).
/// Errors: those of the registry's and the prefabs' readers; NotFound for a family the registry lacks.
[[nodiscard]] Expected<world::SceneData> loadShowcaseData(const std::filesystem::path& assetRoot,
                                                          std::string_view family = kShowcaseFamily);

/// The showcase's games: `recent` in its order (catalog::recentGameBefore: favourites first, then the most recently
/// played), at most `slots`, each completed by `items` (by system) where its system's items file knows it.
[[nodiscard]] std::vector<ShowcaseGame> showcaseGamesOf(std::span<const catalog::RecentGame> recent,
                                                        const std::map<catalog::SystemName, RoomItems>& items,
                                                        std::size_t slots);

/// The showcase's collection: game i on slot i (face-out, the family's item at its face-out size, standing in the
/// slot as a room's display case holds it), pick id i + 1, layer keys "<GameId>/front" and "<GameId>/spine", one cluster
/// (the whole case), one group of the showcase's family. Its room id is empty (the app's keys name the showcase).
[[nodiscard]] world::CollectionLayout showcaseLayout(const HubShowcase& showcase, std::span<const ShowcaseGame> games);

/// The cover loader's items of `layout`'s items (the games in its order): the title, the cover's path, the family's
/// label and spine shapes, a seed from the game (the palette's colour of a label without a cover), the default palette.
[[nodiscard]] std::vector<CoverItem> showcaseCovers(const HubShowcase& showcase, const world::CollectionLayout& layout,
                                                    std::span<const ShowcaseGame> games);

/// What a showcase job made (buildShowcaseContent()).
struct ShowcaseContent {
    std::vector<ShowcaseGame> games;
    world::CollectionLayout layout;
    std::vector<CoverItem> covers;
    /// The favourites could not be read (the showcase stays empty).
    std::optional<Error> error;
    double milliseconds = 0.0;
};

/// The showcase's content (on a worker): the catalog's favourite and recent games read on `reader`'s connection
/// (catalog_db's favoritesAndRecent, the machine's time zone), completed by the items files of their systems
/// (<cacheDir>/world/<library>/room-<system>.json; a file that does not read counts as none), on `showcase`'s slots.
[[nodiscard]] ShowcaseContent buildShowcaseContent(const std::filesystem::path& cacheDir,
                                                   const catalog::LibraryId& library, WorldCatalogReader& reader,
                                                   const HubShowcase& showcase);

} // namespace rv::app
