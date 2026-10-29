#include <rv/app/game_screens.hpp>

#include <algorithm>
#include <span>
#include <utility>

#include <rv/assets/image_io.hpp>
#include <rv/assets/mip_chain.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/room_entry.hpp>
#include <rv/core/file_io.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/ui/media_viewer_model.hpp>
#include <rv/world/title_key.hpp>

namespace rv::app {

std::vector<MediaPageFile> viewerPagesOf(const catalog_db::GameDetails& details) {
    const catalog::CatalogEntry& entry = details.indexed.entry;
    if (entry.key.empty()) {
        return {};
    }
    // One entry's snapshot, so that its paths are derived exactly as the room's are (the extension's checks included).
    catalog::RoomSnapshot snapshot;
    snapshot.system = entry.system;
    catalog::RoomEntry room;
    room.id = details.indexed.id;
    room.key = entry.key;
    room.directory = entry.file ? entry.file->directory : entry.state == catalog::EntryState::Folder;
    room.media = details.indexed.media;
    snapshot.entries.push_back(std::move(room));
    const std::vector<esde::RoomMediaPaths> paths = esde::roomMediaPaths(snapshot);
    if (paths.empty()) {
        return {};
    }
    std::vector<MediaPageFile> pages;
    for (const std::string_view kind : ui::media_viewer::kKinds) {
        const std::optional<catalog::MediaKind> parsed = catalog::parseMediaKind(kind);
        if (!parsed) {
            continue;
        }
        if (const std::string& path = paths.front()[*parsed]; !path.empty()) {
            pages.push_back(MediaPageFile{std::string(kind), path});
        }
    }
    return pages;
}

Expected<std::filesystem::path> viewerMediaRoot(const std::filesystem::path& libraryFile) {
    auto profile = esde::readLibraryFile(libraryFile);
    if (!profile) {
        return propagate(profile.error());
    }
    // ES-DE's settings as they are now (a missing file: ES-DE's defaults, as the room's snapshot takes them).
    const auto settings = esde::readSettings(profile->esdeHome);
    const esde::ResolvedRoot root = esde::resolveMediaRoot(settings ? *settings : esde::EsdeSettings{}, *profile);
    if (!root.path) {
        return makeError(ErrorCode::NotFound, "the media root cannot be resolved from ES-DE's settings");
    }
    return *root.path;
}

Expected<assets::ImageData> viewerPicture(const std::filesystem::path& mediaRoot, std::string_view relativePath,
                                          int maxSide) {
    auto path = core::extendedLengthPathBelow(mediaRoot, relativePath);
    if (!path) {
        return propagate(path.error());
    }
    auto bytes = core::readFileShared(*path, assets::kMaxImageFileBytes);
    if (!bytes) {
        return propagate(bytes.error());
    }
    auto image =
        assets::decodeUprightImage(std::as_bytes(std::span(bytes->data(), bytes->size())), 4, relativePath);
    if (!image) {
        return propagate(image.error());
    }
    const int longer = std::max(image->width, image->height);
    if (maxSide <= 0 || longer <= maxSide) {
        return std::move(*image);
    }
    const double scale = static_cast<double>(maxSide) / static_cast<double>(longer);
    const int width = std::clamp(static_cast<int>(image->width * scale + 0.5), 1, image->width);
    const int height = std::clamp(static_cast<int>(image->height * scale + 0.5), 1, image->height);
    assets::ImageData reduced = assets::reduceLinear(*image, width, height);
    if (!reduced.isValid()) {
        return makeError(ErrorCode::InvalidArgument, "the picture cannot be reduced", std::string(relativePath));
    }
    return reduced;
}

VersionGroups VersionGroups::of(const world::WorldPlan& plan) {
    VersionGroups groups;
    for (const world::UnplacedGame& unplaced : plan.unplaced) {
        if (unplaced.reason != world::UnplacedReason::DuplicateTitle || !unplaced.representative.isValid() ||
            !unplaced.game.isValid() || unplaced.game == unplaced.representative) {
            continue;
        }
        groups.m_groups[unplaced.representative.value()].push_back(unplaced.game);
        groups.m_representativeOf.emplace(unplaced.game.value(), unplaced.representative);
    }
    for (auto& [representative, duplicates] : groups.m_groups) {
        std::sort(duplicates.begin(), duplicates.end(),
                  [](const catalog::GameId& a, const catalog::GameId& b) { return a.value() < b.value(); });
        duplicates.erase(std::unique(duplicates.begin(), duplicates.end()), duplicates.end());
    }
    return groups;
}

std::vector<catalog::GameId> VersionGroups::othersOf(const catalog::GameId& game) const {
    catalog::GameId representative = game;
    if (const auto found = m_representativeOf.find(game.value()); found != m_representativeOf.end()) {
        representative = found->second;
    }
    const auto group = m_groups.find(representative.value());
    if (group == m_groups.end()) {
        return {};
    }
    std::vector<catalog::GameId> others;
    if (representative != game) {
        others.push_back(representative);
    }
    for (const catalog::GameId& duplicate : group->second) {
        if (duplicate != game) {
            others.push_back(duplicate);
        }
    }
    return others;
}

ui::OtherVersion otherVersionOf(const catalog::GameId& game, const std::optional<catalog_db::GameDetails>& details) {
    ui::OtherVersion version;
    version.gameId = game.toString();
    if (!details) {
        return version;
    }
    const catalog::CatalogEntry& entry = details->indexed.entry;
    version.title = entry.title;
    if (!entry.key.empty()) {
        if (const auto region = world::readVersionTags(world::fileNameOf(entry.key.relativePath())).regionTag) {
            version.region = *region;
        }
    }
    return version;
}

ui::GameInfoContent gameInfoOf(std::string_view gameId, std::string_view fallbackTitle,
                               const std::optional<ui::CatalogEntry>& entry, std::vector<ui::OtherVersion> versions) {
    ui::GameInfoContent content;
    content.gameId = std::string(gameId);
    content.title = std::string(fallbackTitle);
    if (entry) {
        content.details = true;
        if (!entry->title.empty()) {
            content.title = entry->title;
        }
        content.platform = entry->platform;
        content.year = entry->year;
        content.developer = entry->developer;
        content.publisher = entry->publisher;
        content.genre = entry->genre;
        content.players = entry->players;
        content.description = entry->description;
    }
    content.versionCount = static_cast<std::uint32_t>(versions.size());
    if (versions.size() > ui::game_info::kMaxVersions) {
        versions.resize(ui::game_info::kMaxVersions);
    }
    content.versions = std::move(versions);
    return content;
}

} // namespace rv::app
