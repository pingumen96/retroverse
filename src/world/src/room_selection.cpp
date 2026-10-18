#include <rv/world/room_selection.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include <rv/catalog/media.hpp>
#include <rv/core/canonical_json.hpp>
#include <rv/world/title_key.hpp>

#include "data_reader.hpp"
#include "plan_parts.hpp"

namespace rv::world {
namespace {

using detail::DocumentReader;
using detail::ObjectReader;
using Json = nlohmann::json;

constexpr std::int64_t kMaxVersion = 999'999;

Json titleJson(const RoomTitle& title) {
    return Json{
        {"game", title.game.toString()}, {"titleKey", title.titleKey},
        {"sortKey", title.sortKey},      {"favorite", title.favorite},
        {"playCount", title.playCount},  {"rating", title.ratingTenths ? Json(*title.ratingTenths) : Json(nullptr)}};
}

Json documentJson(const RoomSelection& selection, std::vector<UnplacedGame> unplaced) {
    Json titles = Json::array();
    for (const RoomTitle& title : selection.titles) {
        titles.push_back(titleJson(title));
    }
    std::sort(unplaced.begin(), unplaced.end());
    Json games = Json::array();
    for (const UnplacedGame& game : unplaced) {
        games.push_back(detail::unplacedJson(game));
    }
    return Json{{"room", selection.room.toString()},
                {"eligibility", toString(selection.eligibility)},
                {"titleKeyRule", selection.titleKeyRule},
                {"catalogSortKeyVersion", selection.catalogSortKeyVersion},
                {"titles", std::move(titles)},
                {"unplaced", std::move(games)}};
}

RoomTitle readTitle(const ObjectReader& reader) {
    RoomTitle title;
    title.game = reader.parsed<catalog::GameId>("game", catalog::GameId::parse, "a GameId");
    title.titleKey = reader.string("titleKey");
    title.sortKey = reader.string("sortKey");
    title.favorite = reader.boolean("favorite");
    title.playCount = reader.unsignedInteger("playCount");
    if (!reader.value("rating").is_null()) {
        title.ratingTenths = static_cast<int>(reader.integer("rating", 0, 10));
    }
    return title;
}

RoomSelection readDocument(DocumentReader& document, const Json& value) {
    const ObjectReader reader(document, value, "",
                              {"catalogSortKeyVersion", "eligibility", "room", "titleKeyRule", "titles", "unplaced"});
    RoomSelection selection;
    selection.room = reader.parsed<RoomId>("room", RoomId::parse, "a room id (room:<system>)");
    selection.eligibility =
        reader.enumeration<Eligibility>("eligibility", parseEligibility, "one of covered, all, no-cover-in-system");
    selection.titleKeyRule = reader.checkedString("titleKeyRule", isValidRuleName, "a rule name (<name>/<version>)");
    selection.catalogSortKeyVersion =
        static_cast<std::uint32_t>(reader.integer("catalogSortKeyVersion", 1, kMaxVersion));

    const Json& titles = reader.array("titles");
    detail::forEachElement(titles, "/titles", [&](const Json& element, const std::string& pointer) {
        selection.titles.push_back(readTitle(ObjectReader(
            document, element, pointer, {"favorite", "game", "playCount", "rating", "sortKey", "titleKey"})));
    });
    std::set<catalog::GameId> games;
    for (std::size_t i = 0; i < selection.titles.size() && !document.failed(); ++i) {
        if (!games.insert(selection.titles[i].game).second) {
            document.fail(detail::pointerChild("/titles", i), "the game appears twice");
        } else if (i > 0 && !priorityLess(selection.titles[i - 1], selection.titles[i])) {
            document.fail(detail::pointerChild("/titles", i),
                          "out of order: titles are in priority order (favourites, play count, rating, sort key, "
                          "GameId)");
        }
    }

    selection.unplaced = detail::readUnplacedList(reader, "unplaced");
    for (std::size_t i = 0; i < selection.unplaced.size() && !document.failed(); ++i) {
        if (selection.unplaced[i].room != selection.room) {
            document.fail(detail::pointerChild(detail::pointerChild("/unplaced", i), "room"),
                          "an unplaced game of another room");
        }
    }
    return selection;
}

/// A version of a title while selectRoom() groups them.
struct Version {
    const catalog::RoomEntry* entry = nullptr;
    VersionTags tags;
};

/// The representative's order (ADR-0010 §3): the tags, then the lookup key (ordinal) and the GameId.
bool versionLess(const Version& a, const Version& b) noexcept {
    if (versionTagsLess(a.tags, b.tags)) {
        return true;
    }
    if (versionTagsLess(b.tags, a.tags)) {
        return false;
    }
    return std::tie(a.entry->key, a.entry->id) < std::tie(b.entry->key, b.entry->id);
}

} // namespace

Expected<RoomSelection> selectRoom(const RoomInput& input, const WorldConfig& config) {
    const catalog::RoomSnapshot& snapshot = input.snapshot;
    auto room = RoomId::forSystem(snapshot.system);
    if (!room) {
        return propagate(room.error());
    }
    if (!snapshot.mediaKnown) {
        return makeError(
            ErrorCode::Unavailable,
            "the media of the system are not known (" +
                (snapshot.mediaUnknownReason.empty() ? std::string("no reason given") : snapshot.mediaUnknownReason) +
                "): its room decides nothing from them",
            snapshot.system.str());
    }

    // The games, and whether any of them has a cover.
    std::vector<const catalog::RoomEntry*> games;
    std::set<catalog::GameId> ids;
    bool anyCover = false;
    for (const catalog::RoomEntry& entry : snapshot.entries) {
        if (!ids.insert(entry.id).second) {
            return makeError(ErrorCode::InvalidArgument,
                             "the snapshot holds the game " + entry.id.toString() + " twice", snapshot.system.str());
        }
        const bool game = entry.state == catalog::EntryState::Game || entry.state == catalog::EntryState::MissingFile;
        if (game && !entry.hidden) {
            games.push_back(&entry);
            anyCover = anyCover || entry.media[catalog::MediaKind::Cover].present;
        }
    }

    RoomSelection selection;
    selection.room = *room;
    selection.eligibility = config.include == PlacementInclude::All ? Eligibility::All
                            : anyCover                              ? Eligibility::Covered
                                                                    : Eligibility::NoCoverInSystem;
    selection.titleKeyRule = std::string(kTitleKeyRule);
    selection.catalogSortKeyVersion = snapshot.sortKeyVersion;
    const auto unplace = [&](const catalog::RoomEntry& entry, UnplacedReason reason, catalog::GameId representative) {
        selection.unplaced.push_back(UnplacedGame{entry.id, *room, reason, representative});
    };

    // The eligible games by title key (an ordered map: nothing depends on a hash order).
    std::map<std::string, std::vector<Version>> titles;
    for (const catalog::RoomEntry* entry : games) {
        if (entry->state == catalog::EntryState::MissingFile) {
            unplace(*entry, UnplacedReason::Unavailable, {});
            continue;
        }
        if (selection.eligibility == Eligibility::Covered && !entry->media[catalog::MediaKind::Cover].present) {
            unplace(*entry, UnplacedReason::NoCover, {});
            continue;
        }
        auto key = titleKey(entry->title);
        if (!key) {
            Error error = key.error();
            error.message = "the title of " + entry->id.toString() + ": " + error.message;
            return propagate(std::move(error));
        }
        titles[std::move(*key)].push_back(Version{entry, readVersionTags(fileNameOf(entry->key.relativePath()))});
    }

    for (auto& [key, versions] : titles) {
        std::sort(versions.begin(), versions.end(), versionLess);
        const catalog::RoomEntry& representative = *versions.front().entry;
        RoomTitle title;
        title.game = representative.id;
        title.titleKey = key;
        title.sortKey = representative.sortKey;
        for (const Version& version : versions) {
            const catalog::RoomEntry& entry = *version.entry;
            title.favorite = title.favorite || entry.favorite;
            title.playCount += entry.esdePlayCount + entry.sessionCount;
            if (entry.rating && (!title.ratingTenths || entry.rating->tenths > *title.ratingTenths)) {
                title.ratingTenths = entry.rating->tenths;
            }
            if (&entry != &representative) {
                unplace(entry, UnplacedReason::DuplicateTitle, representative.id);
            }
        }
        selection.titles.push_back(std::move(title));
    }
    std::sort(selection.titles.begin(), selection.titles.end(), priorityLess);
    std::sort(selection.unplaced.begin(), selection.unplaced.end());
    return selection;
}

bool priorityLess(const RoomTitle& a, const RoomTitle& b) noexcept {
    // Higher first for the favourite, the play count and the rating (an unrated title, -1, after every rated one).
    const auto rank = [](const RoomTitle& title) {
        return std::tuple(!title.favorite, ~title.playCount, -title.ratingTenths.value_or(-1));
    };
    const auto rankA = rank(a);
    const auto rankB = rank(b);
    if (rankA != rankB) {
        return rankA < rankB;
    }
    return std::tie(a.sortKey, a.game) < std::tie(b.sortKey, b.game);
}

Expected<nlohmann::json> roomSelectionToJson(const RoomSelection& selection) {
    Json document = documentJson(selection, selection.unplaced);
    auto text = core::writeCanonicalJson(document);
    if (!text) {
        return propagate(text.error());
    }
    // Checked with the reader, as the plan's writer does: what is written is always read back.
    DocumentReader reader(*text, "room selection");
    (void)readDocument(reader, document);
    if (reader.failed()) {
        Error error = reader.error();
        error.code = ErrorCode::InvalidArgument;
        return propagate(std::move(error));
    }
    return document;
}

Expected<std::string> writeRoomSelection(const RoomSelection& selection) {
    auto document = roomSelectionToJson(selection);
    if (!document) {
        return propagate(document.error());
    }
    return core::writeCanonicalJson(*document);
}

Expected<RoomSelection> readRoomSelection(std::string_view text, std::string_view context) {
    auto value = core::parseCanonicalJson(text, context);
    if (!value) {
        return propagate(value.error());
    }
    DocumentReader document(text, std::string(context));
    RoomSelection selection = readDocument(document, *value);
    if (document.failed()) {
        return propagate(document.error());
    }
    return selection;
}

} // namespace rv::world
