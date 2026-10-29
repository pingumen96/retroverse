// The real library's arcade room with its cabinets playing their clips (I5 step 4, ADR-0016 §6). Hidden
// ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the
// library before and after, the folders in the RV_REAL_* variables and RV_AUDIO=null:
//   ./scripts/real-library.ps1 -Filter '[real-library][arcade]'
// The whole library is imported into a new catalog under the build tree (build/<preset>/real-library/arcade/data,
// library.json naming the real ES-DE home, install, ROM and media roots), then retroverse.exe of this build (hidden,
// lockstep, --start-room area:arcade):
//   - a first start: the building planned and written, the arcade room shown;
//   - each featured cabinet (the display pairs facing the aisle) with the walker 0.9 m in front of it, 150 frames: the
//     cabinet that attract plays after its 1 s dwell, its clip's first frame, the rectangle its picture fills on the
//     cabinet's portrait screen (ADR-0016 §7);
//   - the first cabinet that played inspected from its Focus pose: its clip in the hand (direct), within the budget.
// The figures are printed (the lines starting "Real arcade:"); the runs' reports and screenshots stay in the build tree
// (they show the user's covers and clips: never committed). The checks hold whatever the collection holds.

#include "support/child_process.hpp"
#include "support/fixture.hpp"

#include <rv/app/cabinet_screens.hpp>
#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/catalog/import_service.hpp>
#include <rv/catalog/reconciliation.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <glm/glm.hpp>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view kRoom = "area:arcade";
/// The walker's eye above its feet, the default window's aspect (Focus poses are framed for it).
constexpr float kEyeHeight = 1.65f;
constexpr float kWindowAspect = 1280.0f / 720.0f;

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

/// retroverse.exe with `arguments` on `data`, hidden, its output in <folder>/<name>.log and its report in
/// <folder>/<name>.json; the report read back.
rv::app::RunReport runApp(const fs::path& folder, const fs::path& data, const std::string& name,
                          std::vector<std::string> arguments) {
    const fs::path report = folder / (name + ".json");
    arguments.insert(arguments.end(), {"--hidden", "--data-dir", rv::core::pathToUtf8(data), "--run-report",
                                       rv::core::pathToUtf8(report)});
    rv::test::ChildRequest request;
    request.executable = rv::test::testProgramsDir() / "retroverse.exe";
    request.arguments = std::move(arguments);
    request.stdoutFile = folder / (name + ".log");
    request.stderrFile = request.stdoutFile;
    request.environment.push_back({"RV_ASSET_ROOT", std::nullopt});
    request.timeout = std::chrono::minutes(15);
    const auto run = rv::test::runChild(request);
    REQUIRE(run.has_value());
    INFO(name << ": exit code " << run->exitCode << (run->timedOut ? " (timed out)" : "") << "; "
              << rv::core::pathToUtf8(request.stdoutFile));
    REQUIRE_FALSE(run->timedOut);
    CHECK(run->exitCode == 0);
    auto read = rv::app::readRunReport(report);
    REQUIRE(read.has_value());
    return std::move(*read);
}

/// A lockstep run of `frames` frames starting in the arcade room, with `extra` arguments.
std::vector<std::string> arcadeArguments(int frames, std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--start-room", std::string(kRoom),    "--exit-after-frames", "--frames",
                                       std::to_string(frames), "--fixed-dt", "0.016667"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// "x,y,z,yaw,pitch" for --spawn.
std::string spawnArgument(const glm::vec3& feet, float yawDeg, float pitchDeg) {
    return fmt::format("{:.9g},{:.9g},{:.9g},{:.9g},{:.9g}", feet.x, feet.y, feet.z, yawDeg, pitchDeg);
}

/// The walker 0.9 m in front of an item's visible face, looking at its middle (in the room's frame).
std::string frontSpawn(const rv::world::CollectionItem& item) {
    const glm::vec3 local(item.slotBox.center().x, item.slotBox.center().y, item.slotBox.max.z);
    const glm::vec3 face(item.slotFrame * glm::vec4(local, 1.0f));
    glm::vec3 out(item.slotFrame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    out.y = 0.0f;
    out = glm::length(out) > 1e-4f ? glm::normalize(out) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 eye(face.x + 0.9f * out.x, kEyeHeight, face.z + 0.9f * out.z);
    const glm::vec3 look = face - eye;
    return spawnArgument(glm::vec3(eye.x, 0.0f, eye.z), rv::core::radiansToDegrees(std::atan2(look.x, -look.z)),
                         rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z))));
}

/// The walker at an item's Focus pose.
std::string focusSpawn(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = kWindowAspect;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
    return spawnArgument(pose.feet, rv::core::radiansToDegrees(pose.yaw), rv::core::radiansToDegrees(pose.pitch));
}

const rv::app::VideoCabinetEventRecord* firstVideo(const rv::app::VideoRecord& video, std::string_view screen) {
    const auto found =
        std::find_if(video.cabinetEvents.begin(), video.cabinetEvents.end(), [&](const auto& event) {
            return event.screen == screen && event.picture == "video";
        });
    return found != video.cabinetEvents.end() ? &*found : nullptr;
}

const rv::app::VideoScreenRecord* screenOf(const rv::app::VideoRecord& video, std::string_view screen) {
    const auto found = std::find_if(video.screens.begin(), video.screens.end(),
                                    [&](const auto& record) { return record.screen == screen; });
    return found != video.screens.end() ? &*found : nullptr;
}

/// The checks every run makes: the arcade room shown, never more decoders than the budget, every thread joined.
void checkVideo(const rv::app::RunReport& report) {
    CHECK(report.world.room == kRoom);
    CHECK(report.video.bound);
    CHECK(report.video.maxOpenDecoders <= 2u);
    CHECK(report.video.maxDecoderThreads <= 2u);
    CHECK(report.video.joinedAtExit);
    REQUIRE(report.audio.has_value());
    CHECK(report.audio->device == "null");
}

} // namespace

TEST_CASE("app/world: the real arcade room's cabinets play their clips", "[.real-library][real-library][arcade]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    rv::catalog::LibraryProfile profile;
    profile.id = rv::catalog::LibraryId::parse("real").value();
    profile.esdeHome = *home;
    profile.installFolder = *install;
    profile.romRoot = *romRoot;
    profile.mediaRoot = *mediaRoot;

    // A new data directory under the build tree, never in the library: library.json and the whole library imported.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "arcade";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    const fs::path data = folder / "data";
    REQUIRE(rv::esde::writeLibraryFile(data / "config" / std::string(rv::esde::kLibraryFileName), profile,
                                       rv::esde::ExistingFile::Replace)
                .has_value());
    rv::core::PathOptions pathOptions;
    pathOptions.userDataOverride = data;
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const rv::core::Paths paths = rv::core::Paths::resolve(pathOptions, environment).value();
    {
        const Clock::time_point start = Clock::now();
        auto database = rv::catalog_db::openCatalogDatabase(rv::catalog_db::databaseFile(paths));
        REQUIRE(database.has_value());
        auto store = rv::catalog_db::SqliteCatalogStore::open(*database);
        REQUIRE(store.has_value());
        const auto settings = rv::esde::readSettings(profile.esdeHome);
        REQUIRE(settings.has_value());
        rv::esde::EsdeLibrarySource source;
        rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
        rv::catalog::IdentityPairing pairing;
        rv::catalog::ImportService service(source, media, **store, &pairing);
        const auto imported = service.run(profile, rv::catalog::ImportOptions{});
        INFO((imported ? std::string() : imported.error().toString()));
        REQUIRE(imported.has_value());
        std::cout << fmt::format("Real arcade: the whole library imported in {:.1f} s\n",
                                 std::chrono::duration<double>(Clock::now() - start).count());
    }

    // The first start in the arcade room: the building planned and written.
    const rv::app::RunReport first = runApp(folder, data, "first", arcadeArguments(30));
    REQUIRE(first.world.bound);
    REQUIRE(first.world.room == kRoom);
    CHECK(first.world.hardFindings == 0);
    checkVideo(first);
    const auto stored = rv::world::readStoredPlan(rv::world::worldFiles(data, profile.id).plan);
    REQUIRE(stored.has_value());
    const rv::world::RoomId room = rv::world::RoomId::parse(kRoom).value();
    const fs::path assets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    auto sceneData = rv::world::loadSceneData(assets, stored->plan, room);
    REQUIRE(sceneData.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, room, *sceneData);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, room, *sceneData, {});
    REQUIRE(layout.has_value());
    REQUIRE(layout->groups.size() == 1u);
    CHECK(layout->groups.front().mediaFamily == "arcade_cabinet");
    std::cout << fmt::format("Real arcade: {} cabinet screen(s) placed; the hero cabinet plays {} ({})\n",
                             layout->items.size(), first.video.gameId, first.video.picture);

    // Attract in front of each featured cabinet.
    std::optional<std::size_t> played;
    std::size_t tried = 0;
    std::size_t playing = 0;
    for (std::size_t index = 0; index < layout->items.size(); ++index) {
        const rv::world::CollectionItem& item = layout->items[index];
        if (item.section != rv::world::Section::Featured) {
            continue;
        }
        ++tried;
        const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
        const std::string name = fmt::format("attract-{}", tried);
        const rv::app::RunReport report =
            runApp(folder, data, name, arcadeArguments(150, {"--spawn", frontSpawn(item), "--screenshot",
                                                             rv::core::pathToUtf8(folder / (name + ".png"))}));
        checkVideo(report);
        const rv::app::VideoCabinetEventRecord* shown = firstVideo(report.video, screen);
        const rv::app::VideoScreenRecord* record = screenOf(report.video, screen);
        std::string rect = "the whole screen";
        if (record != nullptr && record->pictureRect.size() == 4u) {
            rect = fmt::format("({:.3f}, {:.3f}) {:.3f} x {:.3f}", record->pictureRect[0], record->pictureRect[1],
                               record->pictureRect[2], record->pictureRect[3]);
        }
        std::cout << fmt::format("Real arcade: featured cabinet {} ({}): {}\n", tried, item.game.toString(),
                                 shown != nullptr ? fmt::format("its clip from frame {} ({}), its picture {}",
                                                                shown->frame, shown->screenClass, rect)
                                                  : std::string("its cover (no clip shown)"));
        if (shown != nullptr) {
            ++playing;
            CHECK(shown->screenClass == "ambient");
            CHECK(shown->frame >= 60u);
            if (!played) {
                played = index;
            }
        }
        // Never another cabinet than the one in front.
        for (const rv::app::VideoCabinetEventRecord& event : report.video.cabinetEvents) {
            CHECK(event.screen == screen);
        }
    }
    std::cout << fmt::format("Real arcade: attract played on {} of the {} featured cabinet(s)\n", playing, tried);
    REQUIRE(played.has_value());

    // The first that played, inspected from its Focus pose: its clip in the hand.
    const rv::world::CollectionItem& item = layout->items[*played];
    const std::string screen = rv::app::cabinetScreenId(kRoom, item.pickId);
    const fs::path script = folder / "inspect.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, "[{\"fromFrame\": 40, \"toFrame\": 40, \"actions\": {\"interact\": "
                                                  "1}}]\n")
                .has_value());
    const rv::app::RunReport inspected =
        runApp(folder, data, "inspect",
               arcadeArguments(120, {"--spawn", focusSpawn(item, scene->camera), "--input-script",
                                     rv::core::pathToUtf8(script), "--screenshot",
                                     rv::core::pathToUtf8(folder / "inspect.png")}));
    checkVideo(inspected);
    CHECK(inspected.world.interaction.state == "inspect");
    const rv::app::VideoCabinetEventRecord* shown = firstVideo(inspected.video, screen);
    REQUIRE(shown != nullptr);
    CHECK(shown->screenClass == "direct");
    CHECK(shown->frame >= 40u);
    CHECK(inspected.video.selections == 0);
    std::cout << fmt::format("Real arcade: inspected, its clip in the hand from frame {} (interact at 40); the hero "
                             "cabinet played {} throughout\n",
                             shown->frame, inspected.video.gameId);
}
