#pragma once

// Private to rv_cli_core (I3 step 3, I4 step 1): what the worldplan subcommands and where-is share: the plan file's
// default place under the user data directory, the library a plan belongs to, a plan file read and lifted when it is
// of schema 1 (ADR-0013 §5), and a plan's summary as text and as JSON (its floors, its rooms with their kind, floor,
// sections and placement, fixtures by role, slots and titles per section, the reserve left, dressing, lights, unplaced
// games per reason). Header only, so that the subcommands keep one source each.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <rv/catalog/ids.hpp>
#include <rv/cli/command.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/world/building.hpp>
#include <rv/world/lift.hpp>
#include <rv/world/world_plan.hpp>
#include <rv/world/world_store.hpp>

#include "catalog_text.hpp"
#include "library_snapshot.hpp"

namespace rv::cli {

/// The plan file of a library's world: <user data>/worlds/<library id>/world-plan.json (ADR-0010 §9; the world store
/// keeps the backups, the meta sidecar, the overrides and the lock beside it: rv/world/world_store.hpp).
inline std::filesystem::path defaultPlanPath(const core::Paths& paths, const catalog::LibraryId& library) {
    return world::worldFiles(paths.userDataRoot(), library).plan;
}

/// The library named by `--library` (`given`), else the one <user data>/config/library.json records. On failure the
/// message is on stderr and `failure` says how the command exits.
inline std::optional<catalog::LibraryId> libraryOf(CommandContext& context, std::string_view name,
                                                   const std::optional<std::string>& given, ExitCode& failure) {
    if (given) {
        auto library = catalog::LibraryId::parse(*given);
        if (!library) {
            context.err.writeLine(fmt::format("{}: --library: {}", name, library.error().toString()));
            failure = ExitCode::UsageError;
            return std::nullopt;
        }
        return *library;
    }
    auto profile = readRecordedLibrary(context, name, failure);
    if (!profile) {
        return std::nullopt;
    }
    return profile->id;
}

/// A plan file as a building: its plan (lifted when the file is of schema 1), the file's digest and schema.
struct PlanFile {
    world::WorldPlan plan;
    std::string digest;
    std::int64_t fileSchema = world::kPlanSchemaVersion;

    [[nodiscard]] bool lifted() const noexcept { return fileSchema != plan.schemaVersion; }
};

/// A plan read (world::readStoredPlan(), or world::readPlan() of a text read otherwise), a schema-1 plan lifted with
/// the building's data under the asset root. Errors: those of reading the building's data, and the lift's.
inline Expected<PlanFile> liftedPlanFile(const core::Paths& paths, world::StoredPlan stored) {
    PlanFile read;
    read.digest = std::move(stored.digest);
    read.fileSchema = stored.plan.schemaVersion;
    if (stored.plan.schemaVersion == world::kPlanSchemaVersion) {
        read.plan = std::move(stored.plan);
        return read;
    }
    auto building = world::loadBuildingData(paths.assetRoot());
    if (!building) {
        return propagate(building.error());
    }
    auto systems = world::loadSystemsData(paths.assetRoot());
    if (!systems) {
        return propagate(systems.error());
    }
    auto lifted = world::lift(stored.plan, *building, *systems);
    if (!lifted) {
        return propagate(lifted.error());
    }
    read.plan = std::move(*lifted);
    return read;
}

/// Reads a plan file (world::readStoredPlan) and lifts a schema-1 plan with the building's data under the asset root.
/// Errors: those of readStoredPlan() (NotFound: no file; ValidationFailed, ParseError, Unsupported: refused), those of
/// reading the building's data, and the lift's (InvalidArgument: a schema-1 plan that is no building's room).
inline Expected<PlanFile> readPlanFile(const core::Paths& paths, const std::filesystem::path& file) {
    auto stored = world::readStoredPlan(file);
    if (!stored) {
        return propagate(stored.error());
    }
    return liftedPlanFile(paths, std::move(*stored));
}

/// "north row" or "south row".
inline std::string_view rowText(const world::RoomPlacement& placement) {
    return world::rowOf(placement) == world::Row::North ? "north row" : "south row";
}

/// The rooms' ids, ", "-separated ("the building" for none).
inline std::string roomsText(const world::WorldPlan& plan) {
    std::string text;
    for (const world::Room& room : plan.rooms) {
        text += (text.empty() ? "" : ", ") + room.id.toString();
    }
    return text.empty() ? std::string("the building (no room)") : text;
}

/// What a plan holds, counted.
struct PlanSummary {
    struct SectionCount {
        std::uint64_t slots = 0;
        std::uint64_t titles = 0;
        std::uint64_t dressedSlots = 0;

        /// The slots neither a title nor dressing takes: the spine-out section's reserve.
        [[nodiscard]] std::uint64_t freeSlots() const noexcept {
            const std::uint64_t used = titles + dressedSlots;
            return slots > used ? slots - used : 0;
        }
    };
    std::array<SectionCount, 3> sections{}; ///< by world::Section
    std::map<std::string, std::uint64_t> fixturesByRole;
    std::map<std::string, std::uint64_t> lightsByRole;
    std::map<std::string, std::uint64_t> unplacedByReason;
    std::uint64_t dressedLevels = 0;
    std::uint64_t lights = 0;
    std::uint64_t unplaced = 0;
};

inline PlanSummary summarizePlan(const world::WorldPlan& plan) {
    PlanSummary summary;
    // The section of every level, to count placements and dressing by section.
    std::map<std::pair<world::FixtureId, std::uint32_t>, world::Section> sections;
    for (const world::Fixture& fixture : plan.fixtures) {
        ++summary.fixturesByRole[std::string(world::toString(fixture.role))];
        for (const world::FixtureLevel& level : fixture.levels) {
            sections[{fixture.id, level.level}] = level.section;
            summary.sections[static_cast<std::size_t>(level.section)].slots += level.slots;
        }
    }
    for (const world::Placement& placement : plan.placements) {
        const auto found = sections.find({placement.slot.fixture(), placement.slot.level()});
        if (found != sections.end()) {
            ++summary.sections[static_cast<std::size_t>(found->second)].titles;
        }
    }
    for (const world::DressedLevel& dressed : plan.dressing) {
        ++summary.dressedLevels;
        const auto found = sections.find({dressed.level.fixture(), dressed.level.level()});
        if (found != sections.end()) {
            summary.sections[static_cast<std::size_t>(found->second)].dressedSlots += dressed.count;
        }
    }
    for (const world::PlanLight& light : plan.lights) {
        ++summary.lights;
        ++summary.lightsByRole[std::string(world::toString(light.role))];
    }
    for (const world::UnplacedGame& game : plan.unplaced) {
        ++summary.unplaced;
        ++summary.unplacedByReason[std::string(world::toString(game.reason))];
    }
    return summary;
}

/// "a 2, b 3" from a count map (sorted by name); "none" when empty.
inline std::string countsText(const std::map<std::string, std::uint64_t>& counts) {
    std::string text;
    for (const auto& [name, count] : counts) {
        text += fmt::format("{}{} {}", text.empty() ? "" : ", ", name, count);
    }
    return text.empty() ? "none" : text;
}

/// The summary as lines of text, after a first line naming the plan (`heading`). A room's theme pack is the one
/// `resolvedPacks` gives it (the pack it is drawn with now, world::roomThemePack(): ADR-0014 §1), its recorded one
/// named beside when they differ (the next update rewrites it); the recorded one for a room `resolvedPacks` lacks.
inline std::string planSummaryText(const world::WorldPlan& plan, std::string_view digest, std::string_view heading,
                                   const std::map<world::RoomId, std::string>& resolvedPacks = {}) {
    const PlanSummary summary = summarizePlan(plan);
    std::string text = fmt::format("{}\n", heading);
    const auto line = [&](std::string_view label, std::string_view value) {
        text += fmt::format("  {:<17}{}\n", fmt::format("{}:", label), value);
    };
    line("Digest", digest);
    line("Generator", fmt::format("{} ({}, {}, sort keys {})", plan.generatorVersion.toString(), plan.randomAlgorithm,
                                  plan.titleKeyRule, plan.catalogSortKeyVersion));
    line("Library", fmt::format("{}, seed {}, preset {}", plan.library.str(), plan.seed, plan.preset));
    std::string floors;
    for (const world::Floor& floor : plan.floors) {
        std::size_t rooms = 0;
        for (const world::Room& room : plan.rooms) {
            rooms += room.floor == floor.id ? 1 : 0;
        }
        floors += fmt::format("{}{} (order {}, {} room{})", floors.empty() ? "" : ", ", floor.id, floor.order, rooms,
                              rooms == 1 ? "" : "s");
    }
    line("Floors", floors.empty() ? std::string("none") : floors);
    for (const world::Room& room : plan.rooms) {
        const auto resolved = resolvedPacks.find(room.id);
        const std::string pack =
            resolved == resolvedPacks.end() || resolved->second == room.themePack
                ? room.themePack
                : fmt::format("{} (recorded {} until the next update)", resolved->second, room.themePack);
        line("Room", fmt::format("{} ({} v{}): {} x {} x {} mm, {} growth step{}, {}, {}", room.id.toString(),
                                 room.templateId, room.templateVersion, room.maxXMm - room.minXMm,
                                 room.maxZMm - room.minZMm, room.heightMm, room.growthSteps,
                                 room.growthSteps == 1 ? "" : "s", room.mediaFamily, pack));
        std::string sections;
        for (const catalog::SystemName& system : room.sections) {
            sections += (sections.empty() ? "" : ", ") + system.str();
        }
        line("  on floor", fmt::format("{}, {} room of {}, {} at x {} z {} mm, generator {}", room.floor,
                                       world::toString(room.kind), sections, rowText(room.placement),
                                       room.placement.xMm, room.placement.zMm, room.generatorVersion.toString()));
    }
    line("Fixtures", fmt::format("{} ({})", plan.fixtures.size(), countsText(summary.fixturesByRole)));
    for (const world::Section section : {world::Section::Featured, world::Section::Band, world::Section::Spine}) {
        const PlanSummary::SectionCount& count = summary.sections[static_cast<std::size_t>(section)];
        line(fmt::format("{} slots", world::toString(section)),
             fmt::format("{} titles in {} slots, {} free, {} dressed", count.titles, count.slots,
                         count.freeSlots(), count.dressedSlots));
    }
    line("Placements", fmt::format("{}", plan.placements.size()));
    line("Dressing", fmt::format("{} level{}", summary.dressedLevels, summary.dressedLevels == 1 ? "" : "s"));
    line("Lights", fmt::format("{} ({})", summary.lights, countsText(summary.lightsByRole)));
    line("Unplaced", fmt::format("{} ({})", summary.unplaced, countsText(summary.unplacedByReason)));
    return text;
}

inline CatalogJson countsJson(const std::map<std::string, std::uint64_t>& counts) {
    CatalogJson json = CatalogJson::object();
    for (const auto& [name, count] : counts) {
        json[name] = count;
    }
    return json;
}

/// The summary as JSON: digest, versions, library, seed, preset, rooms, fixtures (count and per role), the sections
/// (slots, titles, free and dressed slots), placements, dressed levels, lights (count and per role) and unplaced games
/// (count and per reason).
inline CatalogJson planSummaryJson(const world::WorldPlan& plan, std::string_view digest) {
    const PlanSummary summary = summarizePlan(plan);
    CatalogJson json;
    json["digest"] = digest;
    json["schemaVersion"] = plan.schemaVersion;
    json["generatorVersion"] = plan.generatorVersion.toString();
    json["library"] = plan.library.str();
    json["seed"] = plan.seed;
    json["preset"] = plan.preset;
    CatalogJson floors = CatalogJson::array();
    for (const world::Floor& floor : plan.floors) {
        floors.push_back(CatalogJson{{"id", floor.id}, {"order", floor.order}});
    }
    json["floors"] = std::move(floors);
    CatalogJson rooms = CatalogJson::array();
    for (const world::Room& room : plan.rooms) {
        CatalogJson sections = CatalogJson::array();
        for (const catalog::SystemName& system : room.sections) {
            sections.push_back(system.str());
        }
        rooms.push_back(CatalogJson{{"id", room.id.toString()},
                                    {"kind", world::toString(room.kind)},
                                    {"floor", room.floor},
                                    {"sections", std::move(sections)},
                                    {"placement", CatalogJson{{"xMm", room.placement.xMm},
                                                              {"zMm", room.placement.zMm},
                                                              {"yawHalfTurns", room.placement.yawHalfTurns},
                                                              {"row", rowText(room.placement)}}},
                                    {"generatorVersion", room.generatorVersion.toString()},
                                    {"template", room.templateId},
                                    {"templateVersion", room.templateVersion},
                                    {"widthMm", room.maxXMm - room.minXMm},
                                    {"depthMm", room.maxZMm - room.minZMm},
                                    {"heightMm", room.heightMm},
                                    {"growthSteps", room.growthSteps}});
    }
    json["rooms"] = std::move(rooms);
    json["fixtures"] = CatalogJson{{"count", plan.fixtures.size()}, {"byRole", countsJson(summary.fixturesByRole)}};
    CatalogJson sections = CatalogJson::object();
    for (const world::Section section : {world::Section::Featured, world::Section::Band, world::Section::Spine}) {
        const PlanSummary::SectionCount& count = summary.sections[static_cast<std::size_t>(section)];
        sections[std::string(world::toString(section))] =
            CatalogJson{{"slots", count.slots},
                        {"titles", count.titles},
                        {"free", count.freeSlots()},
                        {"dressed", count.dressedSlots}};
    }
    json["sections"] = std::move(sections);
    json["placements"] = plan.placements.size();
    json["dressedLevels"] = summary.dressedLevels;
    json["lights"] = CatalogJson{{"count", summary.lights}, {"byRole", countsJson(summary.lightsByRole)}};
    json["unplaced"] = CatalogJson{{"count", summary.unplaced}, {"byReason", countsJson(summary.unplacedByReason)}};
    return json;
}

} // namespace rv::cli
