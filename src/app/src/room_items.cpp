#include <rv/app/room_items.hpp>

#include <algorithm>
#include <utility>

#include <fmt/format.h>

#include <rv/core/canonical_json.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/scene/instance_data.hpp>

namespace rv::app {
namespace {

using Json = nlohmann::json;

/// The largest file read back: some 40 bytes of keys and the text of a few thousand items.
constexpr std::uintmax_t kMaxFileBytes = 16ull * 1024ull * 1024ull;

[[nodiscard]] tl::unexpected<Error> parseError(std::string_view context, std::string_view pointer,
                                               std::string message) {
    return makeError(ErrorCode::ParseError, std::move(message), fmt::format("{}:{}", context, pointer));
}

/// The string at `key` of `object` (an object), into `out`.
Status readString(const Json& object, std::string_view key, std::string_view context, const std::string& pointer,
                  std::string& out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return parseError(context, pointer + "/" + std::string(key), "expected a string");
    }
    out = found->get<std::string>();
    return ok();
}

} // namespace

const RoomItemText* RoomItems::find(const catalog::GameId& game) const noexcept {
    const auto found =
        std::lower_bound(items.begin(), items.end(), game,
                         [](const RoomItemText& item, const catalog::GameId& id) { return item.game < id; });
    return found != items.end() && found->game == game ? &*found : nullptr;
}

RoomItems roomItemsOf(std::span<const CoverItem> covers, const world::CollectionLayout& layout) {
    RoomItems items;
    items.items.reserve(covers.size());
    for (const CoverItem& cover : covers) {
        if (items.systemName.empty()) {
            items.systemName = cover.systemName;
        }
        RoomItemText text;
        text.game = cover.game;
        text.title = cover.title;
        text.fallbackTitle = cover.fallbackTitle;
        text.year = cover.year;
        text.coverPath = cover.coverPath;
        const world::CollectionItem* item = layout.findGame(cover.game);
        text.available = item == nullptr || (item->flags & scene::instance_flags::kUnavailable) == 0;
        items.items.push_back(std::move(text));
    }
    std::sort(items.items.begin(), items.items.end(),
              [](const RoomItemText& a, const RoomItemText& b) { return a.game < b.game; });
    items.items.erase(std::unique(items.items.begin(), items.items.end(),
                                  [](const RoomItemText& a, const RoomItemText& b) { return a.game == b.game; }),
                      items.items.end());
    return items;
}

std::vector<world::CollectionTitle> collectionTitlesOf(const RoomItems& items, const world::WorldPlan& plan) {
    std::vector<world::CollectionTitle> titles;
    titles.reserve(plan.placements.size());
    for (const world::Placement& placement : plan.placements) {
        if (const RoomItemText* text = items.find(placement.game)) {
            titles.push_back(world::CollectionTitle{text->game, text->title, text->available});
        } else {
            titles.push_back(world::CollectionTitle{placement.game, std::string(), true});
        }
    }
    std::sort(titles.begin(), titles.end(),
              [](const world::CollectionTitle& a, const world::CollectionTitle& b) { return a.game < b.game; });
    titles.erase(
        std::unique(titles.begin(), titles.end(),
                    [](const world::CollectionTitle& a, const world::CollectionTitle& b) { return a.game == b.game; }),
        titles.end());
    return titles;
}

std::filesystem::path roomItemsFile(const std::filesystem::path& cacheDir, const catalog::LibraryId& library,
                                    const catalog::SystemName& system) {
    return cacheDir / "world" / core::pathFromUtf8(library.str()) /
           core::pathFromUtf8("room-" + system.str() + ".json");
}

Json roomItemsToJson(const RoomItems& items) {
    Json list = Json::array();
    for (const RoomItemText& item : items.items) {
        list.push_back(Json{{"game", item.game.toString()},
                            {"title", item.title},
                            {"fallbackTitle", item.fallbackTitle},
                            {"year", item.year},
                            {"coverPath", item.coverPath},
                            {"available", item.available}});
    }
    Json document{
        {"schemaVersion", kRoomItemsSchemaVersion}, {"systemName", items.systemName}, {"items", std::move(list)}};
    // Only when there are any: a room of a catalog without tags keeps I3's bytes.
    if (!items.platforms.empty()) {
        document["platforms"] = items.platforms;
    }
    return document;
}

Expected<RoomItems> parseRoomItems(const Json& document, std::string_view context) {
    if (!document.is_object()) {
        return parseError(context, "", "expected an object");
    }
    const auto version = document.find("schemaVersion");
    if (version == document.end() || !version->is_number_integer()) {
        return parseError(context, "/schemaVersion", "expected an integer");
    }
    if (version->get<std::int64_t>() != kRoomItemsSchemaVersion) {
        return makeError(
            ErrorCode::Unsupported,
            fmt::format("schema version {} is not {}", version->get<std::int64_t>(), kRoomItemsSchemaVersion),
            fmt::format("{}:/schemaVersion", context));
    }
    RoomItems items;
    if (Status read = readString(document, "systemName", context, "", items.systemName); !read) {
        return propagate(read.error());
    }
    if (const auto platforms = document.find("platforms"); platforms != document.end()) {
        if (!platforms->is_array()) {
            return parseError(context, "/platforms", "expected an array of strings");
        }
        for (std::size_t index = 0; index < platforms->size(); ++index) {
            if (!(*platforms)[index].is_string()) {
                return parseError(context, fmt::format("/platforms/{}", index), "expected a string");
            }
            items.platforms.push_back((*platforms)[index].get<std::string>());
        }
    }
    const auto list = document.find("items");
    if (list == document.end() || !list->is_array()) {
        return parseError(context, "/items", "expected an array");
    }
    items.items.reserve(list->size());
    for (std::size_t index = 0; index < list->size(); ++index) {
        const Json& entry = (*list)[index];
        const std::string pointer = fmt::format("/items/{}", index);
        if (!entry.is_object()) {
            return parseError(context, pointer, "expected an object");
        }
        RoomItemText item;
        std::string game;
        for (const auto& [key, out] : {std::pair<std::string_view, std::string*>{"game", &game},
                                       std::pair<std::string_view, std::string*>{"title", &item.title},
                                       std::pair<std::string_view, std::string*>{"fallbackTitle", &item.fallbackTitle},
                                       std::pair<std::string_view, std::string*>{"year", &item.year},
                                       std::pair<std::string_view, std::string*>{"coverPath", &item.coverPath}}) {
            if (Status read = readString(entry, key, context, pointer, *out); !read) {
                return propagate(read.error());
            }
        }
        auto id = catalog::GameId::parse(game);
        if (!id) {
            return parseError(context, pointer + "/game", "not a GameId: " + id.error().message);
        }
        item.game = *id;
        const auto available = entry.find("available");
        if (available == entry.end() || !available->is_boolean()) {
            return parseError(context, pointer + "/available", "expected true or false");
        }
        item.available = available->get<bool>();
        if (!items.items.empty() && !(items.items.back().game < item.game)) {
            return parseError(context, pointer + "/game", "the games are not in GameId order, each once");
        }
        items.items.push_back(std::move(item));
    }
    return items;
}

Expected<RoomItems> readRoomItems(const std::filesystem::path& file) {
    auto text = core::readTextFile(file, kMaxFileBytes);
    if (!text) {
        return propagate(text.error());
    }
    const std::string context = core::pathToUtf8Lossy(file);
    auto document = core::parseJson(*text, context);
    if (!document) {
        return propagate(document.error());
    }
    return parseRoomItems(*document, context);
}

Expected<bool> writeRoomItems(const std::filesystem::path& file, const RoomItems& items) {
    auto text = core::writeCanonicalJson(roomItemsToJson(items));
    if (!text) {
        return propagate(text.error());
    }
    if (auto existing = core::readTextFile(file, kMaxFileBytes); existing && *existing == *text) {
        return false;
    }
    if (Status written = core::writeTextFileAtomic(file, *text); !written) {
        return propagate(written.error());
    }
    return true;
}

} // namespace rv::app
