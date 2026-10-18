#include <rv/world/generation.hpp>

#include <algorithm>
#include <span>
#include <utility>

#include <nlohmann/json.hpp>

#include <rv/assets/prefab_manifest.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/core/canonical_json.hpp>
#include <rv/core/random.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/seeds.hpp>
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

/// The canonical document of what the generator read (ADR-0010 §8's inputDigest; generation.hpp): the configuration as
/// it shapes the room (reduceMotion only shows it), the world seed, the selection, the template by id and version (a
/// template whose sockets change is a new version), what the plan takes from the platform manifest and the media
/// family, and every prefab as measured.
/// `checkedSelection` false takes the selection's document without the reader's check (the update checks it itself).
Expected<std::string> inputDigest(const GenerationInput& input, const WorldData& data, std::uint64_t seed,
                                  bool checkedSelection = true) {
    auto config = worldConfigToJson(input.config);
    if (!config) {
        return propagate(config.error());
    }
    config->erase("reduceMotion");
    auto selection = checkedSelection ? roomSelectionToJson(input.selection)
                                      : Expected<Json>(detail::roomSelectionDocument(input.selection));
    if (!selection) {
        return propagate(selection.error());
    }
    const MediaFamily& family = data.mediaFamily;
    const Json document{
        {"configuration", std::move(*config)},
        {"generatorVersion", kGeneratorVersion.toString()},
        {"library", input.library.str()},
        {"mediaFamily",
         Json{{"id", family.id},
              {"item", prefabJson(family.item)},
              {"itemWidthMm", family.itemWidthMm},
              {"itemHeightMm", family.itemHeightMm},
              {"itemDepthMm", family.itemDepthMm},
              {"spineOut", slotSpecJson(family.spineOut)},
              {"faceOut", slotSpecJson(family.faceOut)}}},
        {"platform", Json{{"system", data.platform.system.str()},
                          {"mediaFamily", data.platform.mediaFamily},
                          {"themePack", data.platform.themePack},
                          {"console", prefabJson(data.platform.console)}}},
        {"prefabs", prefabCatalogToJson(data.prefabs)},
        {"selection", std::move(*selection)},
        {"template", Json{{"id", data.roomTemplate.id}, {"version", data.roomTemplate.version}}},
        {"worldSeed", seed}};
    return core::canonicalJsonDigest(document);
}

} // namespace

namespace detail {

PrefabSlotSource catalogSlots(const PrefabCatalog& prefabs) {
    return [&prefabs](const PrefabRef& ref) -> Expected<std::vector<PrefabSlotLevel>> {
        const PrefabInfo* info = prefabs.find(ref);
        if (info == nullptr) {
            return makeError(ErrorCode::NotFound, "not among the prefabs the generator read", ref.manifest);
        }
        std::vector<PrefabSlotLevel> levels;
        for (const SlotLevelInfo& level : info->levels) {
            levels.push_back({level.level, level.slots});
        }
        return levels;
    };
}

Status checkGenerationInput(const GenerationInput& input, const WorldData& data) {
    auto room = RoomId::forSystem(data.platform.system);
    if (!room) {
        return propagate(room.error());
    }
    if (input.selection.room != *room) {
        return makeError(ErrorCode::InvalidArgument,
                         "the selection is of " + input.selection.room.toString() + ", the data of " + room->toString(),
                         "generate");
    }
    if (data.mediaFamily.id != data.platform.mediaFamily) {
        return makeError(ErrorCode::InvalidArgument,
                         "media family " + data.mediaFamily.id + " is not the platform's " + data.platform.mediaFamily,
                         "generate");
    }
    if (Status status = checkWorldConfig(input.config); !status) {
        return status;
    }
    std::vector<catalog::GameId> unplaced;
    for (const UnplacedGame& game : input.selection.unplaced) {
        unplaced.push_back(game.game);
    }
    std::sort(unplaced.begin(), unplaced.end());
    for (const RoomTitle& title : input.selection.titles) {
        if (std::binary_search(unplaced.begin(), unplaced.end(), title.game)) {
            return makeError(ErrorCode::InvalidArgument, "game " + title.game.toString() + " is a title and unplaced",
                             "generate");
        }
    }
    return ok();
}

ValidationContext validationContext(const WorldPlan& plan, const GenerationInput& input, const WorldData& data,
                                    const WorldOverrides& pins) {
    ValidationContext validation;
    validation.templates = {data.roomTemplate};
    validation.config = input.config;
    validation.selections = {input.selection};
    validation.prefabSlots = catalogSlots(data.prefabs);
    for (const Room& room : plan.rooms) {
        std::uint32_t lights = 0;
        for (const PlanLight& light : plan.lights) {
            lights += light.id.room() == room.id ? 1 : 0;
        }
        validation.lights.push_back({room.id, lights});
    }
    validation.gamePins = pins.gamePins;
    validation.fixturePins = pins.fixturePins;
    return validation;
}

Expected<std::string> uncheckedInputDigest(const GenerationInput& input, const WorldData& data) {
    return inputDigest(input, data, worldSeed(input.config, input.library), false);
}

} // namespace detail

Expected<std::string> generationInputDigest(const GenerationInput& input, const WorldData& data) {
    return inputDigest(input, data, worldSeed(input.config, input.library));
}

Expected<WorldData> loadWorldData(const std::filesystem::path& assetRoot, const catalog::SystemName& system,
                                  std::string_view templateId) {
    WorldData data;
    auto roomTemplate = loadRoomTemplate(assetRoot, templateId);
    if (!roomTemplate) {
        return propagate(roomTemplate.error());
    }
    data.roomTemplate = std::move(*roomTemplate);
    auto platform = loadPlatformManifest(assetRoot, system);
    if (!platform) {
        return propagate(platform.error());
    }
    data.platform = std::move(*platform);
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

Expected<GenerationResult> generate(const GenerationInput& input, const WorldData& data) {
    if (Status status = detail::checkGenerationInput(input, data); !status) {
        Error error = status.error();
        error.code = ErrorCode::InvalidArgument;
        return propagate(std::move(error));
    }
    auto capacity = collectionCapacity(data.roomTemplate, data.prefabs, data.mediaFamily);
    if (!capacity) {
        return propagate(capacity.error());
    }
    const std::vector<RoomTitle>& titles = input.selection.titles;
    const CollectionCounts counts =
        countCollection(*capacity, titles.size(), input.config.growthReservePermille);
    const std::uint64_t seed = worldSeed(input.config, input.library);
    const RoomId& room = input.selection.room;
    const std::uint64_t seedOfRoom = roomSeed(seed, room);

    auto layout = detail::layOutRoom({data.roomTemplate, data.platform, data.mediaFamily, data.prefabs, room,
                                      seedOfRoom, counts, input.config.decorationDensityPermille});
    if (!layout) {
        return propagate(layout.error());
    }
    detail::Allocation allocation = detail::allocate(
        {*layout, std::span<const RoomTitle>(titles).subspan(0, counts.placed), counts,
         input.config.growthReservePermille, data.roomTemplate.eyeHeightMm, data.mediaFamily, seedOfRoom,
         data.roomTemplate.shelfDressing});

    GenerationResult result;
    WorldPlan& plan = result.plan;
    plan.generatorVersion = kGeneratorVersion;
    plan.randomAlgorithm = std::string(core::kRandomAlgorithm);
    plan.titleKeyRule = input.selection.titleKeyRule;
    plan.catalogSortKeyVersion = input.selection.catalogSortKeyVersion;
    plan.library = input.library;
    plan.seed = seed;
    plan.preset = input.config.preset;
    auto digest = inputDigest(input, data, seed);
    if (!digest) {
        Error error = digest.error();
        error.code = ErrorCode::InvalidArgument;
        return propagate(std::move(error));
    }
    plan.inputDigest = std::move(*digest);
    plan.rooms.push_back(layout->room);
    for (detail::PlacedFixture& placed : layout->fixtures) {
        plan.fixtures.push_back(std::move(placed.fixture));
    }
    plan.placements = std::move(allocation.placements);
    plan.dressing = std::move(allocation.dressing);
    plan.lights = std::move(layout->lights);
    plan.unplaced = input.selection.unplaced;
    for (std::size_t i = counts.placed; i < titles.size(); ++i) {
        plan.unplaced.push_back({titles[i].game, room, UnplacedReason::Capacity, {}});
    }
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
    const PinCheck pins = checkPins(plan, {data.roomTemplate}, input.overrides);
    changes.orphanedPins = pins.orphaned;
    changes.needsConfirmation = pins.unhonoured;
    result.validation = validatePlan(plan, detail::validationContext(plan, input, data, input.overrides));
    return result;
}

} // namespace rv::world
