#include "building_support.hpp"

#include <algorithm>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

namespace rv::smoke {
namespace {

namespace fs = std::filesystem;

/// The assets the runs of this build read (the source tree's in developer builds, the deployed copy otherwise).
fs::path buildAssets() {
    return buildConfig().devAssetRoot ? sourceDir() / "assets" : retroverseExe().parent_path() / "assets";
}

} // namespace

PlannedBuilding::PlannedBuilding(std::string_view caseName, bool consoleRooms)
    : fixture(test::Fixture::materialize("esde/building").value()), dir(freshCaseDir(caseName)), data(dir / "data") {
    cli("library", {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
    cli("import", {"import"});
    if (consoleRooms) {
        const auto preset = core::readTextFile(buildAssets() / "world/presets/retro_museum_cozy.json");
        REQUIRE(preset.has_value());
        if (preset->find("sharedRoomMinTitles") != std::string::npos) {
            REQUIRE(core::writeTextFileAtomic(data / "config" / "world.json", "{\"sharedRoomMinTitles\": 0}\n")
                        .has_value());
        }
    }
    cli("generate", {"worldplan", "generate"});
}

AppRun PlannedBuilding::cli(std::string_view name, std::vector<std::string> arguments) const {
    arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
    AppRun run = runCommand(retroverseExe().parent_path() / "retroverse-cli.exe", dir,
                            std::string("cli_") + std::string(name), arguments);
    INFO(run.describe());
    REQUIRE(run.exitCode == 0);
    return run;
}

world::StoredPlan PlannedBuilding::plan() const {
    auto stored = world::readStoredPlan(world::worldFiles(data, catalog::LibraryId::parse("esde").value()).plan);
    REQUIRE(stored.has_value());
    return std::move(*stored);
}

std::string PlannedBuilding::roomOf(std::string_view system) const {
    const world::StoredPlan stored = plan();
    const auto found = std::find_if(stored.plan.rooms.begin(), stored.plan.rooms.end(), [&](const world::Room& room) {
        return std::any_of(room.sections.begin(), room.sections.end(),
                           [&](const catalog::SystemName& name) { return name.str() == system; });
    });
    INFO("the building's room of " << system);
    REQUIRE(found != stored.plan.rooms.end());
    return found->id.toString();
}

const world::Room& roomIn(const world::WorldPlan& plan, std::string_view room) {
    const auto found = std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                    [&](const world::Room& candidate) { return candidate.id.toString() == room; });
    INFO("the room " << room);
    REQUIRE(found != plan.rooms.end());
    return *found;
}

std::size_t placementsIn(const world::WorldPlan& plan, std::string_view room) {
    return static_cast<std::size_t>(
        std::count_if(plan.placements.begin(), plan.placements.end(), [&](const world::Placement& placement) {
            return placement.slot.fixture().room().toString() == room;
        }));
}

} // namespace rv::smoke
