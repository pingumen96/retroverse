#include <rv/world/generation.hpp>

#include <algorithm>
#include <iterator>
#include <set>
#include <span>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/canonical_json.hpp>
#include <rv/core/random.hpp>
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

/// A system's room as a fresh building generates it, before its placement on the floor.
struct EnteringRoom {
    const RoomSelection* selection = nullptr;
    const WorldData* data = nullptr;
    Classification classification;
    std::uint32_t floorOrder = 0;
};

} // namespace

namespace detail {

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

Status checkRoomInput(const RoomSelection& selection, const WorldData& data, bool recordedFamily) {
    auto room = RoomId::forSystem(data.platform.system);
    if (!room) {
        return propagate(room.error());
    }
    if (selection.room != *room) {
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

Expected<std::string> buildingInputDigest(const BuildingDigestInput& input) {
    auto config = worldConfigToJson(input.config);
    if (!config) {
        return propagate(config.error());
    }
    config->erase("reduceMotion");
    config->erase("lightingProfile");
    Json systems = Json::array();
    Json rooms = Json::array();
    for (const auto& [selection, data] : input.rooms) {
        const catalog::SystemName& system = selection->room.system();
        const auto tags = input.platformTags.find(system);
        const std::vector<std::string> platformTags =
            tags != input.platformTags.end() ? tags->second : std::vector<std::string>{};
        const Classification classification = classify(system, platformTags, input.systems, input.building);
        // What decided the system's place (ADR-0013 §2, §5): its own row, or the row its platform tags matched, or the
        // fallback, the tags then in the row. A system with a row of its own hashes alike whatever tags its caller
        // knows, so that the app (none) and the CLI (the catalog's) write one plan for one catalog.
        Json row{{"row", systemEntryToJson(classification.entry)}, {"source", toString(classification.source)}};
        if (classification.source == ClassificationSource::PlatformTag) {
            row["matched"] = classification.matched.str();
        }
        systems.push_back(std::move(row));
        auto room = roomInputJson(*selection, *data, input.checkedSelections);
        if (!room) {
            return propagate(room.error());
        }
        rooms.push_back(std::move(*room));
    }
    const Json document{{"building", buildingDataToJson(input.building)},
                        {"configuration", std::move(*config)},
                        {"generatorVersion", kGeneratorVersion.toString()},
                        {"library", input.library.str()},
                        {"rooms", std::move(rooms)},
                        {"systems", std::move(systems)},
                        {"worldSeed", input.seed}};
    return core::canonicalJsonDigest(document);
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
                                        std::uint64_t worldSeed, const WorldData& data) {
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
    detail::Allocation allocation = detail::allocate(
        {*layout, std::span<const RoomTitle>(titles).subspan(0, counts.placed), counts, config.growthReservePermille,
         data.roomTemplate.eyeHeightMm, data.mediaFamily, seedOfRoom, data.roomTemplate.shelfDressing});

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
                                           const BuildingData& building) {
    const Classification classification = classify(system, platformTags, systems, building);
    if (!classification.hasRoom()) {
        return makeError(ErrorCode::InvalidArgument,
                         "the system " + system.str() + " gets no room (" +
                             std::string(toString(classification.kind())) + ")",
                         "world/systems.json");
    }
    auto manifest = loadPlatformManifest(assetRoot, system);
    if (manifest || manifest.error().code != ErrorCode::NotFound) {
        return manifest;
    }
    const KindDefaults& defaults = systems.defaultsOf(classification.kind());
    PlatformManifest platform;
    platform.system = system;
    platform.mediaFamily = classification.entry.family;
    platform.themePack = defaults.themePack;
    platform.cartridgePlastic = defaults.cartridgePlastic;
    platform.consolePlastic = defaults.consolePlastic;
    platform.accentPlastic = defaults.accentPlastic;
    platform.console = defaults.console;
    platform.signText = classification.entry.sign;
    return platform;
}

Expected<WorldData> loadWorldData(const std::filesystem::path& assetRoot, const RoomDataRequest& request) {
    WorldData data;
    auto building = loadBuildingData(assetRoot);
    if (!building) {
        return propagate(building.error());
    }
    data.building = std::move(*building);
    auto systems = loadSystemsData(assetRoot);
    if (!systems) {
        return propagate(systems.error());
    }
    data.systems = std::move(*systems);
    auto roomTemplate = loadRoomTemplate(assetRoot, request.templateId);
    if (!roomTemplate) {
        return propagate(roomTemplate.error());
    }
    data.roomTemplate = std::move(*roomTemplate);
    auto platform = resolvePlatform(assetRoot, request.system, request.platformTags, data.systems, data.building);
    if (!platform) {
        return propagate(platform.error());
    }
    data.platform = std::move(*platform);
    // A stored room's family and theme pack are authoritative (ADR-0013 §3).
    if (!request.mediaFamily.empty()) {
        data.platform.mediaFamily = request.mediaFamily;
    }
    if (!request.themePack.empty()) {
        data.platform.themePack = request.themePack;
    }
    auto families = loadMediaFamilies(assetRoot);
    if (!families) {
        return propagate(families.error());
    }
    const MediaFamily* family = families->find(data.platform.mediaFamily);
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
    auto prefabFamilies = assets::loadPrefabFamilies(assetRoot);
    if (!prefabFamilies) {
        return propagate(prefabFamilies.error());
    }
    const std::vector<PrefabRef> refs = prefabsToRead(data.roomTemplate, data.platform.console, data.mediaFamily);
    auto prefabs = loadPrefabCatalog(assetRoot, *prefabFamilies, refs);
    if (!prefabs) {
        return propagate(prefabs.error());
    }
    data.prefabs = std::move(*prefabs);
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
    request.mediaFamily = room.mediaFamily;
    request.themePack = room.themePack;
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
    digest.rooms = {{&input.selection, &data}};
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

    // The rooms entering the building, in the order (floor order, systems.json order, system name).
    std::vector<EnteringRoom> entering;
    std::set<catalog::SystemName> seen;
    for (const RoomSelection& selection : input.selections) {
        if (selection.room.kind() != RoomIdKind::System || selection.room.isExpansion()) {
            return refuse("a selection is of a system's room, room:<system>, not " + selection.room.toString());
        }
        const catalog::SystemName& system = selection.room.system();
        if (!seen.insert(system).second) {
            return refuse("two selections of the system " + system.str());
        }
        const WorldData* data = detail::dataOf(rooms, system);
        if (data == nullptr) {
            return refuse("no room data for " + selection.room.toString());
        }
        if (Status status = detail::checkRoomInput(selection, *data, false); !status) {
            Error error = status.error();
            error.code = ErrorCode::InvalidArgument;
            return propagate(std::move(error));
        }
        if (selection.titleKeyRule != input.selections.front().titleKeyRule) {
            return refuse("the selections follow different title-key rules");
        }
        const auto tags = input.platformTags.find(system);
        EnteringRoom room;
        room.selection = &selection;
        room.data = data;
        room.classification =
            classify(system,
                     tags != input.platformTags.end() ? std::span<const std::string>(tags->second)
                                                      : std::span<const std::string>(),
                     systems, building);
        if (!room.classification.hasRoom()) {
            return refuse("the system " + system.str() + " gets no room (" +
                          std::string(toString(room.classification.kind())) + ")");
        }
        room.floorOrder = building.order(room.classification.floor).value_or(0);
        entering.push_back(std::move(room));
    }
    std::sort(entering.begin(), entering.end(), [](const EnteringRoom& a, const EnteringRoom& b) {
        return std::tie(a.floorOrder, a.classification.entry.order, a.classification.entry.system) <
               std::tie(b.floorOrder, b.classification.entry.order, b.classification.entry.system);
    });

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
    for (const EnteringRoom& entry : entering) {
        auto content = detail::generateRoomContent(*entry.selection, input.config, seed, *entry.data);
        if (!content) {
            return propagate(content.error());
        }
        Room& room = content->rooms.front();
        room.kind = RoomKind::Console;
        room.floor = entry.classification.floor;
        room.sections = {entry.selection->room.system()};
        room.generatorVersion = kGeneratorVersion;
        const auto placement =
            placeEnteringRoom(room, floorRows(plan.rooms, room.floor, building.layout), building.layout);
        if (!placement) {
            // The floor holds no more rooms (ADR-0013 §7): every game of the system is unplaced for capacity, naming
            // the room it would have had.
            for (const RoomTitle& title : entry.selection->titles) {
                plan.unplaced.push_back({title.game, entry.selection->room, UnplacedReason::Capacity, {}});
            }
            for (const UnplacedGame& game : entry.selection->unplaced) {
                plan.unplaced.push_back({game.game, entry.selection->room, UnplacedReason::Capacity, {}});
            }
            continue;
        }
        room.placement = *placement;
        detail::appendRoom(plan, std::move(*content));
    }
    detail::setFloors(plan, building);

    detail::BuildingDigestInput digestInput;
    digestInput.config = input.config;
    digestInput.library = input.library;
    digestInput.seed = seed;
    digestInput.building = building;
    digestInput.systems = systems;
    digestInput.platformTags = input.platformTags;
    for (const EnteringRoom& entry : entering) {
        digestInput.rooms.emplace_back(entry.selection, entry.data);
    }
    std::sort(digestInput.rooms.begin(), digestInput.rooms.end(),
              [](const auto& a, const auto& b) { return a.first->room < b.first->room; });
    std::vector<const WorldData*> datas;
    for (const auto& [selection, data] : digestInput.rooms) {
        datas.push_back(data);
    }
    auto digest = detail::buildingInputDigest(digestInput);
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
    auto after = planDigest(plan);
    if (!after) {
        return propagate(after.error());
    }
    changes.digestAfter = std::move(*after);

    // A fresh plan applies no pin (overrides.hpp): every pin is judged, so one the plan does not honour fails
    // pin-broken and the plan is never saved.
    std::vector<RoomTemplate> templates;
    for (const WorldData* data : datas) {
        templates.push_back(data->roomTemplate);
    }
    const PinCheck pins = checkPins(plan, templates, input.overrides);
    changes.orphanedPins = pins.orphaned;
    changes.needsConfirmation = pins.unhonoured;
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
