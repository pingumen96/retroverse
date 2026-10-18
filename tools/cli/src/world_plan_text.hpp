#pragma once

// Private to rv_cli_core (I3 step 3): what `worldplan generate` and `worldplan show` share: the plan file's default
// place under the user data directory, the library a plan belongs to, and a plan's summary as text and as JSON (its
// room, fixtures by role, slots and titles per section, the reserve left, dressing, lights, unplaced games per reason).
// Header only, so that the subcommands keep one source each.

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

/// The summary as lines of text, after a first line naming the plan (`heading`).
inline std::string planSummaryText(const world::WorldPlan& plan, std::string_view digest, std::string_view heading) {
    const PlanSummary summary = summarizePlan(plan);
    std::string text = fmt::format("{}\n", heading);
    const auto line = [&](std::string_view label, std::string_view value) {
        text += fmt::format("  {:<17}{}\n", fmt::format("{}:", label), value);
    };
    line("Digest", digest);
    line("Generator", fmt::format("{} ({}, {}, sort keys {})", plan.generatorVersion.toString(), plan.randomAlgorithm,
                                  plan.titleKeyRule, plan.catalogSortKeyVersion));
    line("Library", fmt::format("{}, seed {}, preset {}", plan.library.str(), plan.seed, plan.preset));
    for (const world::Room& room : plan.rooms) {
        line("Room", fmt::format("{} ({} v{}): {} x {} x {} mm, {} growth step{}, {}, {}", room.id.toString(),
                                 room.templateId, room.templateVersion, room.maxXMm - room.minXMm,
                                 room.maxZMm - room.minZMm, room.heightMm, room.growthSteps,
                                 room.growthSteps == 1 ? "" : "s", room.mediaFamily, room.themePack));
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
    CatalogJson rooms = CatalogJson::array();
    for (const world::Room& room : plan.rooms) {
        rooms.push_back(CatalogJson{{"id", room.id.toString()},
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
