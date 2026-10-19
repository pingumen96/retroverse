#pragma once

// GPU-free (part of rv_app_catalog): no SDL, OpenGL or RmlUi header is included.
//
// What the media viewer and Informazioni (I3 step 14b; ADR-0012 §5) show of a game, from the world's card and from the
// quick catalog alike: the viewer's pages from the catalog's stored media (presence and extension per kind, the paths
// derived as ES-DE names them: esde::roomMediaPaths), the media root as ES-DE's settings give it now, a page's picture
// read and decoded by Retroverse (never by RmlUi, never written), and Informazioni's content from the catalog's row
// with the other versions of its title the room's plan knows (ADR-0010's duplicate-title entries and their
// representative), each with its region tag as its file name writes it.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/image_data.hpp>
#include <rv/catalog/ids.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/core/expected.hpp>
#include <rv/ui/catalog_source.hpp>
#include <rv/ui/game_info_model.hpp>
#include <rv/world/world_plan.hpp>

namespace rv::app {

/// Where a screen was opened from: the world's card ("card") or the quick catalog ("catalog").
inline constexpr std::string_view kScreenFromCard = "card";
inline constexpr std::string_view kScreenFromCatalog = "catalog";

/// The longest side of a viewer's picture (larger ones are reduced, aspect kept).
inline constexpr int kViewerMaxSide = 2048;

/// One page of the viewer: an image kind (ui::media_viewer::kKinds) and its file below the media root.
struct MediaPageFile {
    std::string kind;
    std::string relativePath;

    bool operator==(const MediaPageFile&) const = default;
};

/// The viewer's pages of `details` (its stored media), in ui::media_viewer::kKinds' order: the kinds present with a
/// usable extension; none for an entry without a key. Mix images, videos, manuals and custom files are not pages.
[[nodiscard]] std::vector<MediaPageFile> viewerPagesOf(const catalog_db::GameDetails& details);

/// The media root ES-DE uses now for the library of `libraryFile` (library.json): its settings read again
/// (esde::readSettings, esde::resolveMediaRoot: MediaDirectory, the library's override, ES-DE's default). Errors: the
/// library file or the settings cannot be read, the root cannot be resolved (NotFound). Reads files only.
[[nodiscard]] Expected<std::filesystem::path> viewerMediaRoot(const std::filesystem::path& libraryFile);

/// The picture of the file `relativePath` below `mediaRoot`: read with core::readFileShared (never written), decoded
/// (PNG or JPEG), RGBA8 as encoded (sRGB), reduced to at most `maxSide` on its longer side in linear light, its aspect
/// kept. Errors: those of reading (NotFound, Unavailable, ...) and decoding (ParseError for a truncated file,
/// Unsupported for what is not an image).
[[nodiscard]] Expected<assets::ImageData> viewerPicture(const std::filesystem::path& mediaRoot,
                                                        std::string_view relativePath, int maxSide = kViewerMaxSide);

/// The other versions of each title of a room's plan: a duplicate-title entry's group is its representative and every
/// duplicate of it.
class VersionGroups {
public:
    VersionGroups() = default;
    [[nodiscard]] static VersionGroups of(const world::WorldPlan& plan);

    /// The other versions of `game`'s title, in the order representative first, then the duplicates by GameId; none
    /// for a game the plan has no duplicate of.
    [[nodiscard]] std::vector<catalog::GameId> othersOf(const catalog::GameId& game) const;
    [[nodiscard]] bool empty() const noexcept { return m_groups.empty(); }

private:
    /// Representative -> its duplicates (sorted); duplicate -> its representative.
    std::map<std::uint64_t, std::vector<catalog::GameId>> m_groups;
    std::map<std::uint64_t, catalog::GameId> m_representativeOf;
};

/// Another version as Informazioni lists it: the catalog's title and the region tag of its file name
/// (world::readVersionTags); the title alone, empty, when the catalog lacks it.
[[nodiscard]] ui::OtherVersion otherVersionOf(const catalog::GameId& game,
                                              const std::optional<catalog_db::GameDetails>& details);

/// Informazioni's content for `gameId`: the catalog's row when it answered (title, platform, year, developer,
/// publisher, genre, players, description), else `fallbackTitle` alone with the details still to come; the other
/// versions (at most ui::game_info::kMaxVersions of them kept) and how many there are.
[[nodiscard]] ui::GameInfoContent gameInfoOf(std::string_view gameId, std::string_view fallbackTitle,
                                             const std::optional<ui::CatalogEntry>& entry,
                                             std::vector<ui::OtherVersion> versions);

} // namespace rv::app
