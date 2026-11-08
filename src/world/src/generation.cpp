#include <rv/world/generation.hpp>

#include <algorithm>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/canonical_json.hpp>
#include <rv/core/random.hpp>
#include <rv/core/sha256.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/seeds.hpp>
#include <rv/world/title_key.hpp>
#include <rv/world/validation.hpp>

#include "allocation.hpp"
#include "fixture_layout.hpp"
#include "generation_parts.hpp"
#include "plan_parts.hpp"

namespace rv::world {
namespace {

using Json = nlohmann::json;

Json prefabJson(const PrefabRef& ref) {
    return Json{{"kind", ref.kind}, {"manifest", ref.manifest}};
}

Json slotSpecJson(const SlotSpec& spec) {
    return Json{{"widthMm", spec.widthMm}, {"heightMm", spec.heightMm}, {"depthMm", spec.depthMm},
                {"gapMm", spec.gapMm},     {"marginMm", spec.marginMm}, {"headroomMm", spec.headroomMm}};
}

/// What a room's inputs are in the inputDigest: its selection, its template by id and version (a template whose
/// sockets change is a new version), what the plan takes from its platform and media family, and every prefab as
/// measured. `checkedSelection` false takes the selection's document without the reader's check (the update checks it
/// itself).
Expected<Json> roomInputJson(const RoomSelection& selection, const WorldData& data, bool checkedSelection) {
    auto document = checkedSelection ? roomSelectionToJson(selection)
                                     : Expected<Json>(detail::roomSelectionDocument(selection));
    if (!document) {
        return propagate(document.error());
    }
    const MediaFamily& family = data.mediaFamily;
    Json familyJson{{"id", family.id},
                    {"item", prefabJson(family.item)},
                    {"itemWidthMm", family.itemWidthMm},
                    {"itemHeightMm", family.itemHeightMm},
                    {"itemDepthMm", family.itemDepthMm},
                    {"spineOut", slotSpecJson(family.spineOut)},
                    {"faceOut", slotSpecJson(family.faceOut)}};
    // How its items stand spine-out shapes its rooms (I6 step 4); a family on its end hashes as before it.
    if (family.spineFacing != SpineFacing::End) {
        familyJson["spineFacing"] = std::string(toString(family.spineFacing));
    }
    // The fixtures a family names shape its rooms (ADR-0013 §4); a family without any (the 16-bit cartridges') hashes
    // as before them. The label and the cover fit only draw the room.
    if (!family.fixtures.empty()) {
        const auto optionalPrefab = [](const std::optional<PrefabRef>& ref) {
            return ref ? prefabJson(*ref) : Json();
        };
        familyJson["fixtures"] = Json{{"heroModule", optionalPrefab(family.fixtures.heroModule)},
                                      {"sideModule", optionalPrefab(family.fixtures.sideModule)},
                                      {"display", optionalPrefab(family.fixtures.display)},
                                      {"wallShelf", optionalPrefab(family.fixtures.wallShelf)}};
    }
    return Json{{"room", selection.room.toString()},
                {"mediaFamily", std::move(familyJson)},
                {"platform", Json{{"system", data.platform.system.str()},
                                  {"mediaFamily", data.platform.mediaFamily},
                                  {"themePack", data.platform.themePack},
                                  {"console", data.platform.console ? prefabJson(*data.platform.console) : Json()}}},
                {"prefabs", prefabCatalogToJson(data.prefabs)},
                {"selection", std::move(*document)},
                {"template", Json{{"id", data.roomTemplate.id}, {"version", data.roomTemplate.version}}}};
}

/// What every room's data of an asset root reads alike, read once for a request and the room its system may take a
/// section of.
struct AssetInputs {
    BuildingData building;
    SystemsData systems;
    /// As the file holds it, before a family stands its fixtures in it.
    RoomTemplate roomTemplate;
    MediaFamilyRegistry families;
    assets::PrefabFamilies prefabFamilies;
};

Expected<AssetInputs> readAssetInputs(const std::filesystem::path& assetRoot, std::string_view templateId) {
    AssetInputs inputs;
    auto building = loadBuildingData(assetRoot);
    if (!building) {
        return propagate(building.error());
    }
    inputs.building = std::move(*building);
    auto systems = loadSystemsData(assetRoot);
    if (!systems) {
        return propagate(systems.error());
    }
    inputs.systems = std::move(*systems);
    auto roomTemplate = loadRoomTemplate(assetRoot, templateId);
    if (!roomTemplate) {
        return propagate(roomTemplate.error());
    }
    inputs.roomTemplate = std::move(*roomTemplate);
    auto families = loadMediaFamilies(assetRoot);
    if (!families) {
        return propagate(families.error());
    }
    inputs.families = std::move(*families);
    auto prefabFamilies = assets::loadPrefabFamilies(assetRoot);
    if (!prefabFamilies) {
        return propagate(prefabFamilies.error());
    }
    inputs.prefabFamilies = std::move(*prefabFamilies);
    return inputs;
}

/// The data of `request`'s room from `inputs`; the prefabs `measured` holds are taken from it rather than read again
/// (readPrefabInfo() gives a prefab alike whichever room reads it).
Expected<WorldData> roomData(const std::filesystem::path& assetRoot, const RoomDataRequest& request,
                             const AssetInputs& inputs, const PrefabCatalog* measured) {
    WorldData data;
    data.building = inputs.building;
    data.systems = inputs.systems;
    data.roomTemplate = inputs.roomTemplate;
    data.kind = request.kind;
    if (request.kind == RoomKind::Console) {
        auto platform = resolvePlatform(assetRoot, request.system, request.platformTags, data.systems, data.building,
                                        request.floor);
        if (!platform) {
            return propagate(platform.error());
        }
        data.platform = std::move(*platform);
    } else {
        // A stored room's floor is recorded (sticky): its kind and floor decide its platform, whatever its first
        // section's classification is now (ADR-0013 §3: a data change never moves or freezes a room). Without a floor
        // the system is classified for one, and a system that gets no room is refused.
        std::string floor = request.floor;
        if (floor.empty()) {
            const Classification classification =
                classify(request.system, request.platformTags, data.systems, data.building);
            if (!classification.hasRoom()) {
                return makeError(ErrorCode::InvalidArgument,
                                 "the system " + request.system.str() + " gets no room (" +
                                     std::string(toString(classification.kind())) + ")",
                                 "world/systems.json");
            }
            floor = request.kind == RoomKind::Arcade ? std::string(kGroundFloor) : classification.floor;
        }
        data.platform = sharedRoomPlatform(request.kind, floor, request.system, data.systems, data.building);
    }
    // A stored room's family is authoritative (ADR-0013 §3); its theme pack is a look, resolved (ADR-0014 §1). A new
    // room takes the last successor of the family its platform names in rooms of its kind (I6 step 4, ADR-0014 §2: a
    // platform file naming a family that has one plans the successor's room, never one the next update would migrate).
    if (!request.mediaFamily.empty()) {
        data.platform.mediaFamily = request.mediaFamily;
    } else if (const MediaFamily* latest = inputs.families.latestSuccessor(data.platform.mediaFamily, request.kind)) {
        data.platform.mediaFamily = latest->id;
    }
    const MediaFamily* family = inputs.families.find(data.platform.mediaFamily);
    if (family == nullptr) {
        return makeError(ErrorCode::NotFound, "no media family " + data.platform.mediaFamily,
                         "world/media_families.json");
    }
    data.mediaFamily = *family;
    // The room stands the family's fixtures in the template's sockets (ADR-0013 §4): every consumer of the data (the
    // capacity, the layout, the update's modules) reads them from the template, and the plan records them by ref.
    data.roomTemplate = withFamilyFixtures(std::move(data.roomTemplate), data.mediaFamily);
    // The family's slots are its item's size in each orientation: a manifest that disagrees would make the shelves'
    // slots wrong for the item drawn in them. A missing item manifest falls back like any prefab.
    if (auto item = assets::loadPrefabManifest(assets::prefabManifestPath(assetRoot, family->item.manifest))) {
        if (Status status = checkItemManifest(*family, *item); !status) {
            return propagate(status.error());
        }
    }
    const std::vector<PrefabRef> refs = prefabsToRead(data.roomTemplate, data.platform.console, data.mediaFamily);
    for (const PrefabRef& ref : refs) {
        if (const PrefabInfo* known = measured != nullptr ? measured->find(ref) : nullptr) {
            data.prefabs.put(*known);
            continue;
        }
        auto info = readPrefabInfo(assetRoot, inputs.prefabFamilies, ref);
        if (!info) {
            Error error = info.error();
            error.message = "prefab " + ref.manifest + " (" + ref.kind + "): " + error.message;
            return propagate(std::move(error));
        }
        data.prefabs.put(std::move(*info));
    }
    return data;
}

/// The unplaced entries of a system whose room its floor had no place for (ADR-0013 §7): every game, for capacity,
/// naming `room`.
void unplaceForCapacity(std::vector<UnplacedGame>& unplaced, const RoomSelection& selection, const RoomId& room) {
    for (const RoomTitle& title : selection.titles) {
        unplaced.push_back({title.game, room, UnplacedReason::Capacity, {}});
    }
    for (const UnplacedGame& game : selection.unplaced) {
        unplaced.push_back({game.game, room, UnplacedReason::Capacity, {}});
    }
}

/// The collection's order of titles (ADR-0010 §6, ADR-0013 §6): section, catalog sort key, title key, GameId.
bool alphabeticalLess(const detail::SectionIndex& index, const RoomTitle& a, const RoomTitle& b) {
    const std::uint32_t sectionA = detail::sectionOf(&index, a.game);
    const std::uint32_t sectionB = detail::sectionOf(&index, b.game);
    return std::tie(sectionA, a.sortKey, a.titleKey, a.game) < std::tie(sectionB, b.sortKey, b.titleKey, b.game);
}

} // namespace

namespace detail {

Room enteringRecord(const RoomId& id, RoomKind kind, const std::string& floor,
                    std::vector<catalog::SystemName> sections, const RoomTemplate& roomTemplate) {
    Room room;
    room.id = id;
    room.kind = kind;
    room.floor = floor;
    room.sections = std::move(sections);
    const Footprint base = interiorFootprint(roomTemplate, 0);
    room.minXMm = base.minXMm;
    room.maxXMm = base.maxXMm;
    room.minZMm = base.minZMm;
    room.maxZMm = base.maxZMm;
    return room;
}

Expected<std::uint64_t> roomTitleLimit(const WorldData& data, const WorldConfig& config) {
    auto capacity = collectionCapacity(data.roomTemplate, data.prefabs, data.mediaFamily);
    if (!capacity) {
        return propagate(capacity.error());
    }
    // countCollection() bisects up to the count it is given: a count no room of a template ever holds.
    constexpr std::uint64_t kBeyondAnyRoom = std::uint64_t{1} << 40;
    return countCollection(*capacity, kBeyondAnyRoom, config.growthReservePermille).placed;
}

std::uint32_t chainRoomsFor(std::uint64_t titles, std::uint64_t limit) noexcept {
    if (titles == 0 || limit == 0) {
        return 1;
    }
    const std::uint64_t rooms = (titles + limit - 1) / limit;
    return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(rooms, 1, kMaxRoomIndex));
}

Expected<Room> expansionRecord(const Room& head, std::uint32_t index, const RoomTemplate& roomTemplate) {
    auto id = head.id.head().inChain(index);
    if (!id) {
        return propagate(id.error());
    }
    Room record = enteringRecord(*id, head.kind, head.floor, head.sections, roomTemplate);
    record.expands = head.id.head();
    return record;
}

std::vector<std::uint32_t> ChainNumbering::numbers(const RoomId& head, std::uint32_t count) const {
    std::vector<std::uint32_t> result;
    std::uint32_t highest = 1;
    if (const auto found = standing.find(head); found != standing.end()) {
        for (const std::uint32_t number : found->second) {
            if (result.size() < count) {
                result.push_back(number);
            }
            highest = std::max(highest, number);
        }
    }
    for (std::uint32_t number = next(head, highest); result.size() < count; ++number) {
        result.push_back(number);
    }
    return result;
}

std::uint32_t ChainNumbering::next(const RoomId& head, std::uint32_t highest) const {
    if (const auto found = standing.find(head); found != standing.end() && !found->second.empty()) {
        highest = std::max(highest, found->second.back());
    }
    if (const auto found = named.find(head); found != named.end()) {
        highest = std::max(highest, found->second);
    }
    return highest + 1;
}

ChainNumbering chainNumbering(const WorldOverrides& overrides, const WorldPlan* previous) {
    ChainNumbering numbering;
    const auto name = [&](const RoomId& room) {
        std::uint32_t& highest = numbering.named.try_emplace(room.head(), 1).first->second;
        highest = std::max(highest, room.index());
    };
    for (const GamePin& pin : overrides.gamePins) {
        name(pin.slot.fixture().room());
    }
    for (const FixturePin& pin : overrides.fixturePins) {
        name(pin.fixture.room());
    }
    if (previous != nullptr) {
        for (const Room& room : previous->rooms) {
            if (room.id.isExpansion()) {
                numbering.standing[room.id.head()].push_back(room.id.index());
            }
        }
        for (auto& [head, numbers] : numbering.standing) {
            std::sort(numbers.begin(), numbers.end());
        }
    }
    return numbering;
}

Status checkChainHeads(const WorldPlan& plan, std::string_view context) {
    std::set<RoomId> rooms;
    for (const Room& room : plan.rooms) {
        rooms.insert(room.id);
    }
    for (const Room& room : plan.rooms) {
        if (room.id.isExpansion() && !rooms.contains(room.id.head())) {
            return makeError(ErrorCode::InvalidArgument,
                             "the plan holds the expansion " + room.id.toString() + " without its head " +
                                 room.id.head().toString() +
                                 " (edited by hand?): a chain is worked on by its head; restore a snapshot "
                                 "('worldplan undo') or generate the building again",
                             std::string(context));
        }
    }
    return ok();
}

bool isStalePin(const GamePin& pin) noexcept {
    return pin.conflict == ConflictReason::RoomGone;
}

bool isStalePin(const FixturePin& pin) noexcept {
    return pin.conflict == ConflictReason::RoomGone;
}

Status placeExpansions(BuildingEntry& entry, const WorldConfig& config, const BuildingData& building,
                       const ChainNumbering& numbering, std::span<const Room> others) {
    std::vector<Room> standing(others.begin(), others.end());
    for (const BuildingRoom& room : entry.rooms) {
        standing.push_back(room.record);
        standing.insert(standing.end(), room.expansions.begin(), room.expansions.end());
    }
    for (BuildingRoom& room : entry.rooms) {
        if (room.stored || room.entered.empty()) {
            continue;
        }
        std::uint64_t titles = 0;
        for (const EnteringSystem* system : room.entered) {
            titles += system->selection->titles.size();
        }
        auto limit = roomTitleLimit(*room.data, config);
        if (!limit) {
            return propagate(limit.error());
        }
        const std::uint32_t rooms = chainRoomsFor(titles, *limit);
        Room head = room.record;
        head.sections.clear();
        for (const EnteringSystem* system : room.entered) {
            head.sections.push_back(system->selection->room.system());
        }
        const std::vector<std::uint32_t> numbers = numbering.numbers(head.id.head(), rooms - 1);
        for (std::size_t next = room.expansions.size(); next < numbers.size(); ++next) {
            auto record = expansionRecord(head, numbers[next], room.data->roomTemplate);
            if (!record) {
                break; // past the largest number of a chain
            }
            const auto placement =
                placeEnteringRoom(*record, floorRows(standing, record->floor, building.layout), building.layout);
            if (!placement) {
                break; // a full floor: the chain takes the expansions that found a place
            }
            record->placement = *placement;
            standing.push_back(*record);
            room.expansions.push_back(std::move(*record));
        }
    }
    return ok();
}

RoomPins ChainPins::of(const RoomId& room) const {
    RoomPins result;
    result.previous = previous;
    result.excluded = excluded;
    if (pins == nullptr) {
        return result;
    }
    for (const GamePin& pin : pins->gamePins) {
        if (pin.slot.fixture().room() == room && !isStalePin(pin)) {
            result.games.push_back(pin);
        }
    }
    for (const FixturePin& pin : pins->fixturePins) {
        if (pin.fixture.room() == room && !isStalePin(pin)) {
            result.fixtures.push_back(pin);
        }
    }
    return result;
}

namespace {

/// Each room's quota of a chain's `titles` titles (chainShares()): equal shares, the first ones a title more when they
/// do not divide; a room whose pinned games (`pinned`, by room) outnumber its share takes them all, and the others
/// share the rest equally, until no sharing room's pins outnumber its share. Some room always shares: the pinned games
/// are among the titles, so they cannot all outnumber their shares.
std::vector<std::size_t> shareQuotas(std::size_t titles, const std::vector<std::size_t>& pinned) {
    const std::size_t count = pinned.size();
    std::vector<bool> full(count, false);
    std::vector<std::size_t> quota(count, 0);
    for (bool changed = true; changed;) {
        changed = false;
        std::size_t rest = titles;
        std::size_t open = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (full[i]) {
                rest -= pinned[i];
            } else {
                ++open;
            }
        }
        std::size_t k = 0;
        for (std::size_t i = 0; i < count && open > 0; ++i) {
            if (full[i]) {
                quota[i] = pinned[i];
                continue;
            }
            quota[i] = rest / open + (k < rest % open ? 1 : 0);
            ++k;
            if (pinned[i] > quota[i]) {
                full[i] = true;
                changed = true;
            }
        }
    }
    return quota;
}

} // namespace

std::vector<RoomSelection> chainShares(const RoomSelection& merged, SectionIndex& index, std::span<const RoomId> rooms,
                                       const ChainPins* pins, std::map<catalog::GameId, std::string>* why) {
    RoomSelection chain = merged;
    // Rule 4 over the chain: a pinned duplicate takes its title's place before the titles are shared out, so that its
    // room's share holds it as a title.
    std::map<catalog::GameId, std::size_t> pinnedShare;
    if (pins != nullptr && pins->pins != nullptr) {
        std::vector<std::pair<GamePin, std::size_t>> gamePins;
        for (const GamePin& pin : pins->pins->gamePins) {
            const auto room = std::find(rooms.begin(), rooms.end(), pin.slot.fixture().room());
            if (room != rooms.end() && !isStalePin(pin)) {
                gamePins.emplace_back(pin, static_cast<std::size_t>(room - rooms.begin()));
            }
        }
        std::sort(gamePins.begin(), gamePins.end(),
                  [](const auto& a, const auto& b) { return a.first.game < b.first.game; });
        std::set<catalog::GameId> pinnedGames;
        for (const auto& [pin, share] : gamePins) {
            pinnedGames.insert(pin.game);
        }
        std::set<catalog::GameId> titleGames;
        for (const RoomTitle& title : chain.titles) {
            titleGames.insert(title.game);
        }
        for (const auto& [pin, share] : gamePins) {
            std::string reason;
            if (titleGames.contains(pin.game) ||
                takeRepresentation(chain, index, pin.game, pinnedGames, titleGames, reason)) {
                pinnedShare.emplace(pin.game, share);
            } else if (why != nullptr) {
                why->emplace(pin.game, std::move(reason));
            }
        }
        std::sort(chain.unplaced.begin(), chain.unplaced.end());
    }
    // The quotas; the pinned titles first, each in its room's share, then the others in the collection's order
    // filling the quotas left, room after room.
    const std::size_t count = rooms.size();
    std::vector<std::size_t> pinned(count, 0);
    for (const auto& [game, share] : pinnedShare) {
        ++pinned[share];
    }
    const std::vector<std::size_t> quota = shareQuotas(chain.titles.size(), pinned);
    std::vector<RoomSelection> shares(count);
    for (std::size_t i = 0; i < count; ++i) {
        shares[i].room = rooms[i];
        shares[i].eligibility = chain.eligibility;
        shares[i].titleKeyRule = chain.titleKeyRule;
        shares[i].catalogSortKeyVersion = chain.catalogSortKeyVersion;
    }
    std::vector<RoomTitle> alphabetical;
    for (const RoomTitle& title : chain.titles) {
        if (const auto share = pinnedShare.find(title.game); share != pinnedShare.end()) {
            shares[share->second].titles.push_back(title);
        } else {
            alphabetical.push_back(title);
        }
    }
    std::sort(alphabetical.begin(), alphabetical.end(),
              [&](const RoomTitle& a, const RoomTitle& b) { return alphabeticalLess(index, a, b); });
    std::size_t next = 0;
    for (std::size_t i = 0; i < count; ++i) {
        while (shares[i].titles.size() < quota[i] && next < alphabetical.size()) {
            shares[i].titles.push_back(alphabetical[next++]);
        }
    }
    for (RoomSelection& share : shares) {
        std::sort(share.titles.begin(), share.titles.end(), priorityLess);
        share.distinctTitles = share.titles.size();
    }
    shares.front().unplaced = chain.unplaced;
    for (UnplacedGame& game : shares.front().unplaced) {
        game.room = rooms.front();
    }
    return shares;
}

Expected<WorldPlan> generateChain(const RoomSelection& merged, const SectionIndex* index, std::span<const Room> records,
                                  const std::vector<catalog::SystemName>& sections, const WorldData& data,
                                  const WorldConfig& config, std::uint64_t worldSeed, const BuildingData& building,
                                  const ChainPins* pins, RoomPinOutcome* outcome) {
    if (records.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a chain without a room", "generate");
    }
    const Room& head = records.front();
    std::vector<RoomId> ids;
    for (const Room& record : records) {
        ids.push_back(record.id);
    }
    // A room alone generates as it always did; a chain over its equal alphabetical shares (ADR-0015 §7).
    SectionIndex sharesIndex = index != nullptr ? *index : SectionIndex{};
    std::vector<RoomSelection> shares;
    std::map<catalog::GameId, std::string> why;
    if (records.size() == 1) {
        shares.push_back(merged);
    } else {
        shares = chainShares(merged, sharesIndex, ids, pins, &why);
    }
    WorldPlan chain;
    for (std::size_t i = 0; i < shares.size(); ++i) {
        RoomPins roomPins = pins != nullptr ? pins->of(ids[i]) : RoomPins{};
        roomPins.chainWhy = why;
        const SectionIndex* roomIndex = index == nullptr ? nullptr : (records.size() == 1 ? index : &sharesIndex);
        auto content = generateRoomContent(shares[i], config, worldSeed, data, roomIndex,
                                           pins != nullptr ? &roomPins : nullptr, outcome);
        if (!content) {
            return propagate(content.error());
        }
        Room& record = content->rooms.front();
        record.kind = head.kind;
        record.floor = head.floor;
        record.sections = sections;
        record.generatorVersion = kGeneratorVersion;
        record.expands = i == 0 ? RoomId{} : head.id;
        record.placement = records[i].placement;
        record.placement = followDepth(record, building.layout);
        // Every unplaced entry of a chain names its head (a share's titles over its room's capacity on a full floor).
        if (i > 0) {
            for (UnplacedGame& game : content->unplaced) {
                game.room = head.id;
            }
        }
        appendRoom(chain, std::move(*content));
    }
    return chain;
}

Expected<WorldPlan> generateEnteredChain(const BuildingRoom& room, const WorldConfig& config, std::uint64_t worldSeed,
                                         const BuildingData& building, const ChainPins* pins,
                                         RoomPinOutcome* outcome) {
    std::vector<catalog::SystemName> sections;
    std::vector<const RoomSelection*> selections;
    for (const EnteringSystem* system : room.entered) {
        sections.push_back(system->selection->room.system());
        selections.push_back(system->selection);
    }
    if (selections.empty()) {
        return makeError(ErrorCode::InvalidArgument, "a room without a section", "generate");
    }
    std::vector<Room> records{room.record};
    records.insert(records.end(), room.expansions.begin(), room.expansions.end());
    if (room.kind == RoomKind::Console && selections.size() == 1) {
        return generateChain(*selections.front(), nullptr, records, sections, *room.data, config, worldSeed, building,
                             pins, outcome);
    }
    SectionIndex index;
    const RoomSelection merged = sectionsSelection(room.id, sections, selections, index);
    return generateChain(merged, &index, records, sections, *room.data, config, worldSeed, building, pins, outcome);
}

PrefabSlotSource catalogSlots(std::vector<const PrefabCatalog*> catalogs) {
    return [catalogs = std::move(catalogs)](const PrefabRef& ref) -> Expected<std::vector<PrefabSlotLevel>> {
        for (const PrefabCatalog* prefabs : catalogs) {
            if (const PrefabInfo* info = prefabs->find(ref)) {
                std::vector<PrefabSlotLevel> levels;
                for (const SlotLevelInfo& level : info->levels) {
                    levels.push_back({level.level, level.slots});
                }
                return levels;
            }
        }
        return makeError(ErrorCode::NotFound, "not among the prefabs the generator read", ref.manifest);
    };
}

const WorldData* dataOf(std::span<const WorldData> rooms, const catalog::SystemName& system) noexcept {
    for (const WorldData& data : rooms) {
        if (data.platform.system == system) {
            return &data;
        }
    }
    return nullptr;
}

Status checkRoomInput(const RoomSelection& selection, const WorldData& data, bool recordedFamily, bool anySection) {
    auto room = RoomId::forSystem(data.platform.system);
    if (!room) {
        return propagate(room.error());
    }
    if (!anySection && selection.room != *room) {
        return makeError(ErrorCode::InvalidArgument,
                         "the selection is of " + selection.room.toString() + ", the data of " + room->toString(),
                         "generate");
    }
    if (!recordedFamily && data.mediaFamily.id != data.platform.mediaFamily) {
        return makeError(ErrorCode::InvalidArgument,
                         "media family " + data.mediaFamily.id + " is not the platform's " + data.platform.mediaFamily,
                         "generate");
    }
    std::vector<catalog::GameId> unplaced;
    for (const UnplacedGame& game : selection.unplaced) {
        unplaced.push_back(game.game);
    }
    std::sort(unplaced.begin(), unplaced.end());
    for (const RoomTitle& title : selection.titles) {
        if (std::binary_search(unplaced.begin(), unplaced.end(), title.game)) {
            return makeError(ErrorCode::InvalidArgument, "game " + title.game.toString() + " is a title and unplaced",
                             "generate");
        }
    }
    return ok();
}

RoomSelection sectionsSelection(const RoomId& room, std::span<const catalog::SystemName> sections,
                                std::span<const RoomSelection* const> selections, SectionIndex& index) {
    RoomSelection merged;
    merged.room = room;
    if (!selections.empty()) {
        merged.eligibility = selections.front()->eligibility;
        merged.titleKeyRule = selections.front()->titleKeyRule;
        merged.catalogSortKeyVersion = selections.front()->catalogSortKeyVersion;
    }
    for (const RoomSelection* selection : selections) {
        const auto position = std::find(sections.begin(), sections.end(), selection->room.system());
        const auto section = static_cast<std::uint32_t>(position - sections.begin());
        for (const RoomTitle& title : selection->titles) {
            merged.titles.push_back(title);
            index[title.game] = section;
        }
        for (const UnplacedGame& game : selection->unplaced) {
            UnplacedGame entry = game;
            entry.room = room;
            merged.unplaced.push_back(entry);
        }
        merged.distinctTitles += selection->distinctTitles;
    }
    std::sort(merged.titles.begin(), merged.titles.end(), priorityLess);
    std::sort(merged.unplaced.begin(), merged.unplaced.end());
    return merged;
}

namespace {

/// The digest's configuration: the world's, but the keys that only draw it.
Expected<Json> digestConfigurationJson(const WorldConfig& config) {
    auto json = worldConfigToJson(config);
    if (!json) {
        return propagate(json.error());
    }
    json->erase("reduceMotion");
    json->erase("lightingProfile");
    json->erase("systemColors");
    return json;
}

/// The digest's rooms by the selections' rooms (room:<system>), whichever room or section each system stands in, so
/// that a fresh building and its update hash alike.
std::vector<std::pair<const RoomSelection*, const WorldData*>> sortedDigestRooms(const BuildingDigestInput& input) {
    std::vector<std::pair<const RoomSelection*, const WorldData*>> sorted = input.rooms;
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first->room < b.first->room; });
    return sorted;
}

/// What decided the system's place (ADR-0013 §2, §5): its own row, or the row its platform tags matched, or the
/// fallback, the tags then in the row. A system with a row of its own hashes alike whatever tags its caller knows (a
/// selection file's none, the app's and the CLI's the catalog's), so that one catalog gives one plan. `groundFloor`
/// set when the system stands on the ground floor.
Json digestSystemRow(const BuildingDigestInput& input, const RoomSelection& selection, bool& groundFloor) {
    const catalog::SystemName& system = selection.room.system();
    const auto tags = input.platformTags.find(system);
    const std::vector<std::string> platformTags =
        tags != input.platformTags.end() ? tags->second : std::vector<std::string>{};
    const Classification classification = classify(system, platformTags, input.systems, input.building);
    groundFloor = groundFloor || classification.floor == kGroundFloor;
    Json row{{"row", systemEntryToJson(classification.entry)}, {"source", toString(classification.source)}};
    if (classification.source == ClassificationSource::PlatformTag) {
        row["matched"] = classification.matched.str();
    }
    return row;
}

/// The building's document, the hub's layout in it when a room of the ground floor is among the rooms (I6 step 5), so
/// that the other floors' plans keep their provenance.
Json digestBuildingJson(const BuildingData& building, bool groundFloor) {
    Json json = buildingDataToJson(building);
    if (groundFloor) {
        json["hub"] = hubLayoutToJson(building.layout.hub);
    }
    return json;
}

/// roomInputJson()'s members taken from the room's data, written once per data as nested texts.
struct DigestDataTexts {
    std::string mediaFamily;
    std::string platform;
    std::string prefabs;
    std::string roomTemplate;
};

Expected<DigestDataTexts> digestDataTexts(const WorldData& data) {
    // roomInputJson() of an empty selection: the same members of the data.
    auto room = roomInputJson(RoomSelection{}, data, false);
    if (!room) {
        return propagate(room.error());
    }
    DigestDataTexts texts;
    const std::pair<const char*, std::string*> members[] = {{"mediaFamily", &texts.mediaFamily},
                                                            {"platform", &texts.platform},
                                                            {"prefabs", &texts.prefabs},
                                                            {"template", &texts.roomTemplate}};
    for (const auto& [key, text] : members) {
        auto written = core::nestedCanonicalText(room->at(key));
        if (!written) {
            return propagate(written.error());
        }
        *text = std::move(*written);
    }
    return texts;
}

/// roomSelectionDocument()'s text written straight from a selection whose form checkSelectionForm() accepted (its
/// unplaced games sorted already). Mirror any change in room_selection.cpp's documentJson() and titleJson().
void writeSelectionText(core::CanonicalTextWriter& out, const RoomSelection& selection) {
    out.beginObject();
    out.key("catalogSortKeyVersion");
    out.unsignedInteger(selection.catalogSortKeyVersion);
    out.key("distinctTitles");
    out.unsignedInteger(selection.distinctTitles);
    out.key("eligibility");
    out.string(toString(selection.eligibility));
    out.key("room");
    out.string(selection.room.toString());
    out.key("titleKeyRule");
    out.string(selection.titleKeyRule);
    out.key("titles");
    out.beginArray();
    for (const RoomTitle& title : selection.titles) {
        out.beginObject();
        out.key("favorite");
        out.boolean(title.favorite);
        out.key("game");
        out.string(title.game.toString());
        out.key("playCount");
        out.unsignedInteger(title.playCount);
        out.key("rating");
        if (title.ratingTenths) {
            out.integer(*title.ratingTenths);
        } else {
            out.null();
        }
        out.key("sortKey");
        out.string(title.sortKey);
        out.key("titleKey");
        out.string(title.titleKey);
        out.endObject();
    }
    out.endArray();
    out.key("unplaced");
    out.beginArray();
    for (const UnplacedGame& game : selection.unplaced) {
        out.beginObject();
        out.key("game");
        out.string(game.game.toString());
        out.key("reason");
        out.string(toString(game.reason));
        if (game.reason == UnplacedReason::DuplicateTitle || game.representative.isValid()) {
            out.key("representative");
            out.string(game.representative.toString());
        }
        out.key("room");
        out.string(game.room.toString());
        out.endObject();
    }
    out.endArray();
    out.endObject();
}

} // namespace

Status checkSelectionForm(const RoomSelection& selection, std::string_view context) {
    const auto refuse = [&](std::string message) {
        return makeError(ErrorCode::InvalidArgument, std::move(message), std::string(context));
    };
    if (!isValidRuleName(selection.titleKeyRule) || selection.catalogSortKeyVersion < 1 ||
        selection.catalogSortKeyVersion > 999'999 || selection.room.empty()) {
        return refuse("the selection's room, title-key rule or sort-key version is malformed");
    }
    if (selection.distinctTitles < selection.titles.size()) {
        return refuse("the selection counts fewer distinct titles than titles");
    }
    std::vector<catalog::GameId> games;
    games.reserve(selection.titles.size());
    for (std::size_t i = 0; i < selection.titles.size(); ++i) {
        const RoomTitle& title = selection.titles[i];
        if (!title.game.isValid() || (title.ratingTenths && (*title.ratingTenths < 0 || *title.ratingTenths > 10))) {
            return refuse("a title of the selection is malformed: " + title.game.toString());
        }
        if (i > 0 && !priorityLess(selection.titles[i - 1], title)) {
            return refuse("the selection's titles are not in priority order at " + title.game.toString());
        }
        games.push_back(title.game);
    }
    std::sort(games.begin(), games.end());
    if (std::adjacent_find(games.begin(), games.end()) != games.end()) {
        return refuse("a game is twice among the selection's titles");
    }
    for (std::size_t i = 0; i < selection.unplaced.size(); ++i) {
        const UnplacedGame& game = selection.unplaced[i];
        const bool duplicate = game.reason == UnplacedReason::DuplicateTitle;
        if (!game.game.isValid() || game.room != selection.room || duplicate != game.representative.isValid() ||
            (i > 0 && !(selection.unplaced[i - 1] < game))) {
            return refuse("an unplaced game of the selection is malformed or out of order: " + game.game.toString());
        }
    }
    return ok();
}

namespace {

/// The inputs' document (the digest's path through documents, as before m3/preview-time). Mirror any change in
/// writeBuildingInputText(), which writes its text straight from the inputs (the regeneration tests compare both on
/// every world fixture: detail::ScopedDocumentDigests). Errors: those of roomSelectionToJson() (the reader's check of
/// a selection) and of the documents of the configuration and the building.
Expected<Json> buildingInputDocument(const BuildingDigestInput& input) {
    auto config = digestConfigurationJson(input.config);
    if (!config) {
        return propagate(config.error());
    }
    Json systems = Json::array();
    Json rooms = Json::array();
    bool groundFloor = false;
    for (const auto& [selection, data] : sortedDigestRooms(input)) {
        systems.push_back(digestSystemRow(input, *selection, groundFloor));
        auto room = roomInputJson(*selection, *data, input.checkedSelections);
        if (!room) {
            return propagate(room.error());
        }
        rooms.push_back(std::move(*room));
    }
    return Json{{"building", digestBuildingJson(input.building, groundFloor)},
                {"configuration", std::move(*config)},
                {"generatorVersion", kGeneratorVersion.toString()},
                {"library", input.library.str()},
                {"rooms", std::move(rooms)},
                {"systems", std::move(systems)},
                {"worldSeed", input.seed}};
}

/// The canonical text of buildingInputDocument() appended to `out` straight from the inputs: the selections' titles
/// and unplaced games, most of the text, written without their documents, each room's data and the building's as
/// documents (one per room data, however many systems share it). Mirror any change in buildingInputDocument().
/// Errors (the caller then takes the document's path, which names the problem or makes the same digest): a selection
/// that checkSelectionForm() refuses, a string that is not UTF-8.
Status writeBuildingInputText(const BuildingDigestInput& input, std::string& out) {
    auto config = digestConfigurationJson(input.config);
    if (!config) {
        return propagate(config.error());
    }
    const auto sorted = sortedDigestRooms(input);
    // The selections' form first: one the reader's check would refuse goes through the document, which names it.
    for (const auto& [selection, data] : sorted) {
        if (Status form = checkSelectionForm(*selection, "inputDigest"); !form) {
            return form;
        }
    }
    Json systems = Json::array();
    bool groundFloor = false;
    for (const auto& [selection, data] : sorted) {
        systems.push_back(digestSystemRow(input, *selection, groundFloor));
    }
    std::map<const WorldData*, DigestDataTexts> dataTexts;
    core::CanonicalTextWriter text(out);
    text.beginObject();
    text.key("building");
    text.value(digestBuildingJson(input.building, groundFloor));
    text.key("configuration");
    text.value(*config);
    text.key("generatorVersion");
    text.string(kGeneratorVersion.toString());
    text.key("library");
    text.string(input.library.str());
    text.key("rooms");
    text.beginArray();
    for (const auto& [selection, data] : sorted) {
        auto found = dataTexts.find(data);
        if (found == dataTexts.end()) {
            auto texts = digestDataTexts(*data);
            if (!texts) {
                return propagate(texts.error());
            }
            found = dataTexts.emplace(data, std::move(*texts)).first;
        }
        const DigestDataTexts& texts = found->second;
        text.beginObject();
        text.key("mediaFamily");
        text.raw(texts.mediaFamily);
        text.key("platform");
        text.raw(texts.platform);
        text.key("prefabs");
        text.raw(texts.prefabs);
        text.key("room");
        text.string(selection->room.toString());
        text.key("selection");
        writeSelectionText(text, *selection);
        text.key("template");
        text.raw(texts.roomTemplate);
        text.endObject();
    }
    text.endArray();
    text.key("systems");
    text.beginArray();
    for (const Json& row : systems) {
        text.value(row);
    }
    text.endArray();
    text.key("worldSeed");
    text.unsignedInteger(input.seed);
    text.endObject();
    return text.finish();
}

} // namespace

Expected<std::string> buildingInputDigest(const BuildingDigestInput& input) {
    if (!documentDigests()) {
        std::string text;
        if (writeBuildingInputText(input, text)) {
            return core::sha256Hex(text);
        }
        // The document names what the direct text could not take, or makes the same digest.
    }
    auto document = buildingInputDocument(input);
    if (!document) {
        return propagate(document.error());
    }
    return core::canonicalJsonDigest(*document);
}

Expected<std::string> overridesDigestTerm(const WorldOverrides& honoured) {
    if (honoured.empty()) {
        return std::string();
    }
    std::vector<const GamePin*> gamePins;
    for (const GamePin& pin : honoured.gamePins) {
        gamePins.push_back(&pin);
    }
    std::sort(gamePins.begin(), gamePins.end(), [](const GamePin* a, const GamePin* b) {
        return std::tie(a->game, a->slot) < std::tie(b->game, b->slot);
    });
    std::vector<const FixturePin*> fixturePins;
    for (const FixturePin& pin : honoured.fixturePins) {
        fixturePins.push_back(&pin);
    }
    std::sort(fixturePins.begin(), fixturePins.end(),
              [](const FixturePin* a, const FixturePin* b) { return a->fixture < b->fixture; });
    Json games = Json::array();
    for (const GamePin* pin : gamePins) {
        games.push_back(Json{{"game", pin->game.toString()}, {"slot", pin->slot.toString()}});
    }
    Json fixtures = Json::array();
    for (const FixturePin* pin : fixturePins) {
        Json json{{"fixture", pin->fixture.toString()}};
        if (pin->prefab) {
            json["prefab"] = Json{{"kind", pin->prefab->kind}, {"manifest", pin->prefab->manifest}};
        }
        if (pin->transform) {
            json["transform"] = Json{
                {"xMm", pin->transform->xMm}, {"yawCdeg", pin->transform->yawCdeg}, {"zMm", pin->transform->zMm}};
        }
        fixtures.push_back(std::move(json));
    }
    return core::canonicalJsonDigest(Json{{"fixturePins", std::move(fixtures)}, {"gamePins", std::move(games)}});
}

Expected<std::string> joinDigestTerm(std::string_view digest, std::string_view term) {
    if (!isSha256Hex(digest) || (!term.empty() && !isSha256Hex(term))) {
        return makeError(ErrorCode::InvalidArgument, "a digest to join is not 64 lowercase hexadecimal digits",
                         "inputDigest");
    }
    if (term.empty()) {
        return std::string(digest);
    }
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string joined(digest);
    for (std::size_t i = 0; i < joined.size(); ++i) {
        const std::size_t a = kDigits.find(digest[i]);
        const std::size_t b = kDigits.find(term[i]);
        joined[i] = kDigits[a ^ b];
    }
    return joined;
}

Expected<std::string> joinOverridesDigest(std::string_view digest, const WorldOverrides& honoured) {
    auto term = overridesDigestTerm(honoured);
    if (!term) {
        return propagate(term.error());
    }
    return joinDigestTerm(digest, *term);
}

ValidationContext buildingValidationContext(const WorldPlan& plan, const WorldConfig& config,
                                            std::span<const RoomSelection> selections,
                                            std::span<const WorldData* const> rooms, const BuildingData& building,
                                            const WorldOverrides& pins, std::vector<KeptRoom> keptRooms) {
    ValidationContext validation;
    std::vector<const PrefabCatalog*> catalogs;
    for (const WorldData* data : rooms) {
        const bool known = std::any_of(validation.templates.begin(), validation.templates.end(),
                                       [&](const RoomTemplate& candidate) {
                                           return candidate.id == data->roomTemplate.id &&
                                                  candidate.version == data->roomTemplate.version;
                                       });
        if (!known) {
            validation.templates.push_back(data->roomTemplate);
        }
        catalogs.push_back(&data->prefabs);
    }
    validation.config = config;
    validation.selections.assign(selections.begin(), selections.end());
    validation.prefabSlots = catalogSlots(std::move(catalogs));
    for (const Room& room : plan.rooms) {
        std::uint32_t lights = 0;
        for (const PlanLight& light : plan.lights) {
            lights += light.id.room() == room.id ? 1 : 0;
        }
        validation.lights.push_back({room.id, lights});
    }
    validation.gamePins = pins.gamePins;
    validation.fixturePins = pins.fixturePins;
    validation.building = building;
    validation.keptRooms = std::move(keptRooms);
    return validation;
}

Expected<WorldPlan> generateRoomContent(const RoomSelection& selection, const WorldConfig& config,
                                        std::uint64_t worldSeed, const WorldData& data, const SectionIndex* sections,
                                        const RoomPins* pins, RoomPinOutcome* outcome) {
    if (pins != nullptr && !pins->empty()) {
        RoomPinOutcome ignored;
        return generatePinnedRoom(selection, config, worldSeed, data, sections, *pins,
                                  outcome != nullptr ? *outcome : ignored);
    }
    auto capacity = collectionCapacity(data.roomTemplate, data.prefabs, data.mediaFamily);
    if (!capacity) {
        return propagate(capacity.error());
    }
    const std::vector<RoomTitle>& titles = selection.titles;
    const CollectionCounts counts = countCollection(*capacity, titles.size(), config.growthReservePermille);
    const RoomId& room = selection.room;
    const std::uint64_t seedOfRoom = roomSeed(worldSeed, room);

    auto layout = detail::layOutRoom({data.roomTemplate, data.platform, data.mediaFamily, data.prefabs, room,
                                      seedOfRoom, counts, config.decorationDensityPermille});
    if (!layout) {
        return propagate(layout.error());
    }
    detail::Allocation allocation =
        detail::allocate({*layout, std::span<const RoomTitle>(titles).subspan(0, counts.placed), counts,
                          config.growthReservePermille, data.roomTemplate.eyeHeightMm, data.mediaFamily, seedOfRoom,
                          data.roomTemplate.shelfDressing, sections});

    WorldPlan plan;
    plan.rooms.push_back(layout->room);
    for (detail::PlacedFixture& placed : layout->fixtures) {
        plan.fixtures.push_back(std::move(placed.fixture));
    }
    plan.placements = std::move(allocation.placements);
    plan.dressing = std::move(allocation.dressing);
    plan.lights = std::move(layout->lights);
    plan.unplaced = selection.unplaced;
    for (std::size_t i = counts.placed; i < titles.size(); ++i) {
        plan.unplaced.push_back({titles[i].game, room, UnplacedReason::Capacity, {}});
    }
    return plan;
}

Expected<EnteringSystem> enteringSystem(const RoomSelection& selection, std::span<const WorldData> rooms,
                                        const std::map<catalog::SystemName, std::vector<std::string>>& tags,
                                        const SystemsData& systems, const BuildingData& building) {
    const auto refuse = [](std::string message) {
        return makeError(ErrorCode::InvalidArgument, std::move(message), "generate");
    };
    if (selection.room.kind() != RoomIdKind::System || selection.room.isExpansion()) {
        return refuse("a selection is of a system's room, room:<system>, not " + selection.room.toString());
    }
    const catalog::SystemName& system = selection.room.system();
    const WorldData* data = dataOf(rooms, system);
    if (data == nullptr) {
        return refuse("no room data for " + selection.room.toString());
    }
    if (Status status = checkRoomInput(selection, *data, false); !status) {
        Error error = status.error();
        error.code = ErrorCode::InvalidArgument;
        return propagate(std::move(error));
    }
    EnteringSystem entering;
    entering.selection = &selection;
    entering.data = data;
    const auto found = tags.find(system);
    entering.classification = classify(
        system, found != tags.end() ? std::span<const std::string>(found->second) : std::span<const std::string>(),
        systems, building);
    if (!entering.classification.hasRoom()) {
        return refuse("the system " + system.str() + " gets no room (" +
                      std::string(toString(entering.classification.kind())) + ")");
    }
    entering.floorOrder = building.order(entering.classification.floor).value_or(0);
    return entering;
}

Expected<BuildingEntry> enterBuilding(std::vector<BuildingRoom> stored, std::vector<EnteringSystem>& entering,
                                      const WorldConfig& config, const BuildingData& building) {
    std::sort(entering.begin(), entering.end(), [](const EnteringSystem& a, const EnteringSystem& b) {
        return std::tie(a.floorOrder, a.classification.entry.order, a.classification.entry.system) <
               std::tie(b.floorOrder, b.classification.entry.order, b.classification.entry.system);
    });
    BuildingEntry entry;
    entry.rooms = std::move(stored);
    const auto rowsOf = [&](std::string_view floor) {
        std::vector<Room> records;
        for (const BuildingRoom& room : entry.rooms) {
            records.push_back(room.record);
        }
        return floorRows(records, floor, building.layout);
    };
    const auto standing = [&](const RoomId& id) -> BuildingRoom* {
        const auto found = std::find_if(entry.rooms.begin(), entry.rooms.end(),
                                        [&](const BuildingRoom& room) { return room.id == id; });
        return found != entry.rooms.end() ? &*found : nullptr;
    };
    // A section of a standing shared or arcade room, appended unless the room is closed (the system then waits).
    const auto join = [&](BuildingRoom& room, const EnteringSystem& system) {
        if (room.closed) {
            entry.waiting.emplace_back(&system, room.id);
            return;
        }
        room.entered.push_back(&system);
        entry.digest.emplace_back(system.selection, room.data);
    };
    // The systems enter in that order (ADR-0013 §3, §7): each takes its own room, or a section of its floor's shared
    // room or of the arcade room; a room is placed when it enters, so that the rooms after it stand east of it.
    for (const EnteringSystem& system : entering) {
        const catalog::SystemName& name = system.selection->room.system();
        RoomKind kind = RoomKind::Console;
        Expected<RoomId> id = system.selection->room;
        std::string floor = system.classification.floor;
        if (system.classification.kind() == SystemKind::Arcade) {
            kind = RoomKind::Arcade;
            id = RoomId::arcadeArea();
            floor = std::string(kGroundFloor);
        } else if (system.selection->distinctTitles < static_cast<std::uint64_t>(config.sharedRoomMinTitles)) {
            kind = RoomKind::Shared;
            id = RoomId::sharedArea(floor);
        }
        if (!id) {
            return propagate(id.error());
        }

        if (kind == RoomKind::Console) {
            Room record = detail::enteringRecord(*id, kind, floor, {name}, system.data->roomTemplate);
            if (const auto placement = placeEnteringRoom(record, rowsOf(floor), building.layout)) {
                record.placement = *placement;
                entry.rooms.push_back(BuildingRoom{*id, kind, floor, system.data, std::move(record), false, false,
                                                   {&system}});
                entry.digest.emplace_back(system.selection, system.data);
                continue;
            }
            // The floor holds no more rooms (ADR-0013 §7): the system joins its floor's shared room when it stands;
            // else every game of the system is unplaced for capacity, naming the room it would have had.
            const auto shared = RoomId::sharedArea(floor);
            if (BuildingRoom* sharedRoom = shared ? standing(*shared) : nullptr) {
                join(*sharedRoom, system);
                continue;
            }
            unplaceForCapacity(entry.capacity, *system.selection, *id);
            entry.digest.emplace_back(system.selection, system.data);
            continue;
        }

        if (BuildingRoom* room = standing(*id)) {
            join(*room, system);
            continue;
        }
        // The first section of a shared or arcade room: the room enters its floor.
        const WorldData* data = system.data->sharedRoom.get();
        if (data == nullptr || data->kind != kind) {
            return makeError(ErrorCode::InvalidArgument,
                             "no data of " + id->toString() + " for " + name.str() +
                                 " (loadWorldData() of a system reads the room its section would stand in)",
                             "generate");
        }
        Room record = detail::enteringRecord(*id, kind, floor, {name}, data->roomTemplate);
        entry.digest.emplace_back(system.selection, data);
        if (const auto placement = placeEnteringRoom(record, rowsOf(floor), building.layout)) {
            record.placement = *placement;
            entry.rooms.push_back(BuildingRoom{*id, kind, floor, data, std::move(record), false, false, {&system}});
            continue;
        }
        unplaceForCapacity(entry.capacity, *system.selection, *id);
    }
    return entry;
}

Expected<WorldPlan> generateEnteredRoom(const BuildingRoom& room, const WorldConfig& config, std::uint64_t worldSeed,
                                        const BuildingData& building, const RoomPins* pins, RoomPinOutcome* outcome) {
    std::vector<catalog::SystemName> sections;
    std::vector<const RoomSelection*> selections;
    for (const EnteringSystem* system : room.entered) {
        sections.push_back(system->selection->room.system());
        selections.push_back(system->selection);
    }
    Expected<WorldPlan> content = makeError(ErrorCode::InvalidArgument, "a room without a section", "generate");
    if (room.kind == RoomKind::Console && selections.size() == 1) {
        content = generateRoomContent(*selections.front(), config, worldSeed, *room.data, nullptr, pins, outcome);
    } else {
        SectionIndex index;
        const RoomSelection merged = sectionsSelection(room.id, sections, selections, index);
        content = generateRoomContent(merged, config, worldSeed, *room.data, &index, pins, outcome);
    }
    if (!content) {
        return propagate(content.error());
    }
    Room& record = content->rooms.front();
    record.kind = room.kind;
    record.floor = room.floor;
    record.sections = std::move(sections);
    record.generatorVersion = kGeneratorVersion;
    record.placement = room.record.placement;
    record.placement = followDepth(record, building.layout);
    return content;
}

void appendRoom(WorldPlan& plan, WorldPlan&& room) {
    const auto move = [](auto& into, auto& from) {
        into.insert(into.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
    };
    move(plan.rooms, room.rooms);
    move(plan.fixtures, room.fixtures);
    move(plan.placements, room.placements);
    move(plan.dressing, room.dressing);
    move(plan.lights, room.lights);
    move(plan.unplaced, room.unplaced);
}

void setFloors(WorldPlan& plan, const BuildingData& building) {
    std::set<std::string> ids{std::string(kGroundFloor)};
    for (const Room& room : plan.rooms) {
        ids.insert(room.floor);
    }
    plan.floors.clear();
    for (const std::string& id : ids) {
        if (auto floor = building.planFloor(id)) {
            plan.floors.push_back(std::move(*floor));
        }
    }
    std::sort(plan.floors.begin(), plan.floors.end(),
              [](const Floor& a, const Floor& b) { return std::tie(a.order, a.id) < std::tie(b.order, b.id); });
}

GeneratorVersion highestGeneratorVersion(const WorldPlan& plan) {
    if (plan.rooms.empty()) {
        return kGeneratorVersion;
    }
    GeneratorVersion highest = plan.rooms.front().generatorVersion;
    for (const Room& room : plan.rooms) {
        highest = std::max(highest, room.generatorVersion);
    }
    return highest;
}

} // namespace detail

Expected<PlatformManifest> resolvePlatform(const std::filesystem::path& assetRoot, const catalog::SystemName& system,
                                           std::span<const std::string> platformTags, const SystemsData& systems,
                                           const BuildingData& building, std::string_view floor) {
    const Classification classification = classify(system, platformTags, systems, building);
    if (!classification.hasRoom()) {
        return makeError(ErrorCode::InvalidArgument,
                         "the system " + system.str() + " gets no room (" +
                             std::string(toString(classification.kind())) + ")",
                         "world/systems.json");
    }
    // The pack of the floor its room stands on (a stored room's recorded one; else its classification's), unless its
    // manifest names one (ADR-0014 §1 as amended).
    const std::string floorPack =
        themePackOf(RoomKind::Console, floor.empty() ? std::string_view(classification.floor) : floor,
                    classification.kind(), systems, building);
    auto manifest = loadPlatformManifest(assetRoot, system);
    if (manifest) {
        if (manifest->themePack.empty()) {
            manifest->themePack = floorPack;
        }
        return manifest;
    }
    if (manifest.error().code != ErrorCode::NotFound) {
        return manifest;
    }
    const KindDefaults& defaults = systems.defaultsOf(classification.kind());
    PlatformManifest platform;
    platform.system = system;
    platform.mediaFamily = classification.entry.family;
    platform.themePack = floorPack;
    platform.cartridgePlastic = defaults.cartridgePlastic;
    platform.consolePlastic = defaults.consolePlastic;
    platform.accentPlastic = defaults.accentPlastic;
    platform.console = defaults.console;
    platform.signText = classification.entry.sign;
    return platform;
}

Expected<std::string> roomThemePack(const std::filesystem::path& assetRoot, const Room& room,
                                    std::span<const std::string> platformTags, const SystemsData& systems,
                                    const BuildingData& building) {
    const catalog::SystemName system = room.sections.empty() ? room.id.system() : room.sections.front();
    if (room.kind != RoomKind::Console) {
        return sharedRoomPlatform(room.kind, room.floor, system, systems, building).themePack;
    }
    auto platform = resolvePlatform(assetRoot, system, platformTags, systems, building, room.floor);
    if (!platform) {
        return propagate(platform.error());
    }
    return std::move(platform->themePack);
}

PlatformManifest sharedRoomPlatform(RoomKind kind, std::string_view floor, const catalog::SystemName& firstSection,
                                    const SystemsData& systems, const BuildingData& building) {
    SystemKind defaultsKind = SystemKind::Arcade;
    if (kind != RoomKind::Arcade) {
        const FloorSpec* spec = building.floor(floor);
        defaultsKind = spec != nullptr && !spec->kinds.empty() ? spec->kinds.front() : SystemKind::Other;
    }
    const KindDefaults& defaults = systems.defaultsOf(defaultsKind);
    PlatformManifest platform;
    platform.system = firstSection;
    platform.mediaFamily = std::string(kind == RoomKind::Arcade ? kArcadeRoomFamily : kSharedRoomFamily);
    platform.themePack = themePackOf(kind, floor, defaultsKind, systems, building);
    platform.cartridgePlastic = defaults.cartridgePlastic;
    platform.consolePlastic = defaults.consolePlastic;
    platform.accentPlastic = defaults.accentPlastic;
    // The hero corner's console part is left out by kind (ADR-0013 §6): a room of several systems shows none.
    platform.console = std::nullopt;
    platform.signText = defaults.sign.empty() ? systems.fallback.sign : defaults.sign;
    return platform;
}

Expected<WorldData> loadWorldData(const std::filesystem::path& assetRoot, const RoomDataRequest& request) {
    auto inputs = readAssetInputs(assetRoot, request.templateId);
    if (!inputs) {
        return propagate(inputs.error());
    }
    auto data = roomData(assetRoot, request, *inputs, nullptr);
    if (!data) {
        return propagate(data.error());
    }
    // A system entering the building may take a section rather than a room of its own (ADR-0013 §3): the data of that
    // room comes with its own, the prefabs both name measured once.
    if (request.kind == RoomKind::Console && request.mediaFamily.empty()) {
        const Classification classification =
            classify(request.system, request.platformTags, inputs->systems, inputs->building);
        RoomDataRequest shared = request;
        shared.kind = classification.kind() == SystemKind::Arcade ? RoomKind::Arcade : RoomKind::Shared;
        shared.floor = shared.kind == RoomKind::Arcade ? std::string(kGroundFloor) : classification.floor;
        auto sharedData = roomData(assetRoot, shared, *inputs, &data->prefabs);
        if (!sharedData) {
            Error error = sharedData.error();
            error.message = "the data of the room its section would stand in: " + error.message;
            return propagate(std::move(error));
        }
        data->sharedRoom = std::make_shared<const WorldData>(std::move(*sharedData));
    }
    // A stored room of a family that has a successor in rooms of its kind (I6 step 4, ADR-0014 §2): the data of its
    // migration comes with its own, the same room with the last successor of its family (its template kept, its theme
    // pack resolved on its floor as its own).
    if (!request.mediaFamily.empty() && inputs->families.successorOf(request.mediaFamily, request.kind) != nullptr) {
        const MediaFamily* latest = inputs->families.latestSuccessor(request.mediaFamily, request.kind);
        RoomDataRequest migrated = request;
        migrated.mediaFamily = latest->id;
        auto successor = roomData(assetRoot, migrated, *inputs, &data->prefabs);
        if (!successor) {
            Error error = successor.error();
            error.message = "the data of its migration to " + latest->id + ": " + error.message;
            return propagate(std::move(error));
        }
        data->successor = std::make_shared<const WorldData>(std::move(*successor));
    }
    return data;
}

Expected<WorldData> loadWorldData(const std::filesystem::path& assetRoot, const catalog::SystemName& system,
                                  std::string_view templateId) {
    RoomDataRequest request;
    request.system = system;
    request.templateId = std::string(templateId);
    return loadWorldData(assetRoot, request);
}

Expected<WorldData> loadRoomData(const std::filesystem::path& assetRoot, const Room& room,
                                 std::span<const std::string> platformTags) {
    if (room.sections.empty() && room.id.system().empty()) {
        return makeError(ErrorCode::InvalidArgument, "the room holds no system", room.id.toString());
    }
    RoomDataRequest request;
    request.system = room.sections.empty() ? room.id.system() : room.sections.front();
    request.platformTags.assign(platformTags.begin(), platformTags.end());
    request.kind = room.kind;
    request.floor = room.floor;
    request.mediaFamily = room.mediaFamily;
    request.templateId = room.templateId;
    return loadWorldData(assetRoot, request);
}

Expected<std::string> generationInputDigest(const GenerationInput& input, const WorldData& data) {
    detail::BuildingDigestInput digest;
    digest.config = input.config;
    digest.library = input.library;
    digest.seed = worldSeed(input.config, input.library);
    digest.building = data.building;
    digest.systems = data.systems;
    // The data of the room the system enters, as generate() takes it: a section's is its shared or arcade room's.
    const bool arcade = classify(input.selection.room.system(), {}, data.systems, data.building).kind() ==
                        SystemKind::Arcade;
    const bool section =
        arcade || input.selection.distinctTitles < static_cast<std::uint64_t>(input.config.sharedRoomMinTitles);
    digest.rooms = {{&input.selection, section && data.sharedRoom ? data.sharedRoom.get() : &data}};
    return detail::buildingInputDigest(digest);
}

Expected<GenerationResult> generateBuilding(const BuildingInput& input, const BuildingData& building,
                                            const SystemsData& systems, std::span<const WorldData> rooms) {
    const auto refuse = [](std::string message) {
        return makeError(ErrorCode::InvalidArgument, std::move(message), "generate");
    };
    if (Status status = checkWorldConfig(input.config); !status) {
        Error error = status.error();
        error.code = ErrorCode::InvalidArgument;
        return propagate(std::move(error));
    }
    const std::uint64_t seed = worldSeed(input.config, input.library);

    // The systems entering the building.
    std::vector<detail::EnteringSystem> entering;
    std::set<catalog::SystemName> seen;
    for (const RoomSelection& selection : input.selections) {
        if (selection.room.kind() == RoomIdKind::System && !seen.insert(selection.room.system()).second) {
            return refuse("two selections of the system " + selection.room.system().str());
        }
        if (selection.titleKeyRule != input.selections.front().titleKeyRule) {
            return refuse("the selections follow different title-key rules");
        }
        auto system = detail::enteringSystem(selection, rooms, input.platformTags, systems, building);
        if (!system) {
            return propagate(system.error());
        }
        entering.push_back(std::move(*system));
    }

    GenerationResult result;
    WorldPlan& plan = result.plan;
    plan.generatorVersion = kGeneratorVersion;
    plan.randomAlgorithm = std::string(core::kRandomAlgorithm);
    plan.titleKeyRule = input.selections.empty() ? std::string(kTitleKeyRule) : input.selections.front().titleKeyRule;
    plan.catalogSortKeyVersion =
        input.selections.empty() ? catalog::kSortKeyVersion : input.selections.front().catalogSortKeyVersion;
    plan.library = input.library;
    plan.seed = seed;
    plan.preset = input.config.preset;
    // The systems enter in the order (floor order, systems.json order, system name), each taking its own room or a
    // section of its floor's shared room or of the arcade room (ADR-0013 §3, §7); then every room is generated.
    auto entry = detail::enterBuilding({}, entering, input.config, building);
    if (!entry) {
        return propagate(entry.error());
    }
    // The rooms past their template's capacity take expansions (ADR-0015 §7), placed once every system entered, their
    // numbers after those the overrides name.
    const detail::ChainNumbering numbering = detail::chainNumbering(input.overrides);
    if (Status placed = detail::placeExpansions(*entry, input.config, building, numbering); !placed) {
        return propagate(placed.error());
    }
    for (const detail::BuildingRoom& room : entry->rooms) {
        auto content = detail::generateEnteredChain(room, input.config, seed, building);
        if (!content) {
            return propagate(content.error());
        }
        detail::appendRoom(plan, std::move(*content));
        result.changes.roomsAdded.push_back(room.id);
        for (const Room& expansion : room.expansions) {
            result.changes.roomsAdded.push_back(expansion.id);
        }
        for (const detail::EnteringSystem* system : room.entered) {
            result.changes.systemsEntered.push_back({system->selection->room.system(), room.id});
        }
    }
    plan.unplaced.insert(plan.unplaced.end(), entry->capacity.begin(), entry->capacity.end());
    detail::setFloors(plan, building);

    detail::BuildingDigestInput digestInput;
    digestInput.rooms = std::move(entry->digest);
    digestInput.config = input.config;
    digestInput.library = input.library;
    digestInput.seed = seed;
    digestInput.building = building;
    digestInput.systems = systems;
    digestInput.platformTags = input.platformTags;
    std::sort(digestInput.rooms.begin(), digestInput.rooms.end(),
              [](const auto& a, const auto& b) { return a.first->room < b.first->room; });
    // The rooms' data, once each, in the selections' order: their templates and measured prefabs judge the plan.
    std::vector<const WorldData*> datas;
    for (const auto& [selection, data] : digestInput.rooms) {
        if (std::find(datas.begin(), datas.end(), data) == datas.end()) {
            datas.push_back(data);
        }
    }
    // A fresh plan applies no pin (overrides.hpp): every pin is judged, so one the plan does not honour fails
    // pin-broken and the plan is never saved. The pins it honours join its inputDigest (ADR-0015 §2).
    std::vector<RoomTemplate> templates;
    for (const WorldData* data : datas) {
        templates.push_back(data->roomTemplate);
    }
    const PinCheck pins = checkPins(plan, templates, input.overrides);
    auto digest = detail::buildingInputDigest(digestInput);
    if (digest) {
        digest = detail::joinOverridesDigest(*digest, pins.honoured);
    }
    if (!digest) {
        Error error = digest.error();
        error.code = ErrorCode::InvalidArgument;
        return propagate(std::move(error));
    }
    plan.inputDigest = std::move(*digest);
    sortPlan(plan);

    ChangeReport& changes = result.changes;
    changes.outcome = GenerationOutcome::Generated;
    changes.rebuild = RebuildLevel::Full;
    for (const Placement& placement : plan.placements) {
        changes.added.push_back(placement.game);
    }
    std::sort(changes.added.begin(), changes.added.end());
    for (const UnplacedGame& game : plan.unplaced) {
        changes.unplaced.push_back(game.game);
    }
    changes.unplaced.erase(std::unique(changes.unplaced.begin(), changes.unplaced.end()), changes.unplaced.end());
    for (const Fixture& fixture : plan.fixtures) {
        changes.fixturesAdded.push_back(fixture.id);
    }
    std::sort(changes.roomsAdded.begin(), changes.roomsAdded.end());
    std::sort(changes.systemsEntered.begin(), changes.systemsEntered.end());
    auto after = planDigest(plan);
    if (!after) {
        return propagate(after.error());
    }
    changes.digestAfter = std::move(*after);

    changes.orphanedPins = pins.orphaned;
    changes.needsConfirmation = pins.unhonoured;
    changes.needsConfirmationRooms = pins.unhonouredRooms;
    result.validation =
        validatePlan(plan, detail::buildingValidationContext(plan, input.config, input.selections, datas, building,
                                                             input.overrides, {}));
    return result;
}

Expected<GenerationResult> generate(const GenerationInput& input, const WorldData& data) {
    BuildingInput building;
    building.library = input.library;
    building.selections = {input.selection};
    building.config = input.config;
    building.overrides = input.overrides;
    return generateBuilding(building, data.building, data.systems, std::span<const WorldData>(&data, 1));
}

} // namespace rv::world
