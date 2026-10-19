// The persisted start and the room's hero CRT in retroverse.exe (I3 step 10c), on the snes-room fixture
// (tests/fixtures/esde/snes-room: 50 invented titles with generated covers and clips) recorded as the library of the
// case's data directory and imported with retroverse-cli. A run with a plan stored shows the stored plan's room at its
// first frame, before the catalog's snapshot is even asked for (its covers and spines from the derived-texture cache by
// the room's items the last run kept), and reconciles it with the snapshot once that frame is shown: an unchanged
// catalog moves and rebuilds nothing and writes nothing (the frames differ only on the CRT, which now plays the start
// choice), a game added rebuilds the room around the items that stood, the walker where it stands. The room's CRT plays
// the ES-DE catalog's clips: the start choice (the first featured title with a video), and the quick catalog's
// selection, damaged clips falling back down the chain to the cover. A manifest removed from a scratch copy of the
// asset root falls back to its family's prefab, warned once, the room built. Every run is hidden and lockstep
// (--fixed-dt).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/room_items.hpp>
#include <rv/app/run_report.hpp>
#include <rv/assets/prefab_resolver.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "image_checks.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
/// A pixel differs when a channel moves by more than this (retroverse-screenshot-diff's default).
constexpr int kPixelThreshold = 6;

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The snes-room fixture recorded as the library of <caseDir>/data and imported with retroverse-cli.
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedRoom(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("world_start/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    /// retroverse-cli --data-dir <data> `arguments`, which must exit 0.
    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    /// The room's items the runs keep for the persisted start.
    [[nodiscard]] fs::path itemsFile() const {
        return rv::app::roomItemsFile(data / "cache", rv::catalog::LibraryId::parse("esde").value(),
                                      rv::catalog::SystemName::parse("snes").value());
    }

    /// The GameId of the placed game titled `title`, from the room's items.
    [[nodiscard]] std::string gameOf(std::string_view title) const {
        auto items = rv::app::readRoomItems(itemsFile());
        REQUIRE(items.has_value());
        const auto found = std::find_if(items->items.begin(), items->items.end(),
                                        [title](const rv::app::RoomItemText& item) { return item.title == title; });
        INFO("the placed game titled " << title);
        REQUIRE(found != items->items.end());
        return found->game.toString();
    }
};

/// A lockstep run of the room: `frames` frames at a fixed step, a screenshot at the end when `shot` names one.
std::vector<std::string> roomArguments(std::string_view frames, const std::optional<fs::path>& shot = std::nullopt,
                                       std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--exit-after-frames", "--frames", std::string(frames), "--fixed-dt",
                                       "0.016667"};
    if (shot) {
        arguments.insert(arguments.end(), {"--screenshot", arg(*shot)});
    }
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// Writes <dir>/<name> with the input script `steps` and returns it.
fs::path writeScript(const fs::path& dir, std::string_view name, const Json& steps) {
    const fs::path file = dir / rv::core::pathFromUtf8(name);
    REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    return file;
}

/// The room's scene as the run made it from its stored plan (the assets of its build).
rv::scene::SceneDescription shownScene(const ImportedRoom& room, const rv::app::RunReport& report) {
    const auto library = rv::catalog::LibraryId::parse(report.world.library);
    REQUIRE(library.has_value());
    auto stored = rv::world::readStoredPlan(rv::world::worldFiles(room.data, *library).plan);
    REQUIRE(stored.has_value());
    const rv::world::RoomId roomId = rv::world::RoomId::parse(report.world.room).value();
    auto data = rv::world::loadSceneData(report.assetRoot, stored->plan, roomId);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, roomId, *data);
    REQUIRE(scene.has_value());
    return std::move(*scene);
}

/// Where the room's hero CRT (the scene's crt prefab) appears in a kShotSize screenshot from the scene's camera, with a
/// margin for the bloom its screen casts around it.
std::pair<glm::ivec2, glm::ivec2> crtRegion(const rv::scene::SceneDescription& scene) {
    constexpr int kBloomMargin = 96;
    const auto crt = std::find_if(scene.objects.begin(), scene.objects.end(), [](const rv::scene::SceneObject& object) {
        return object.source == rv::scene::SceneObject::Source::Prefab && object.kind == "crt";
    });
    REQUIRE(crt != scene.objects.end());
    auto prefab = rv::geometry::buildPrefab(crt->kind, crt->params);
    REQUIRE(prefab.has_value());
    const rv::core::Aabb bounds = prefab->bounds.transformed(crt->transform);
    glm::ivec2 min(kShotSize);
    glm::ivec2 max(0);
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point((corner & 1) != 0 ? bounds.max.x : bounds.min.x,
                              (corner & 2) != 0 ? bounds.max.y : bounds.min.y,
                              (corner & 4) != 0 ? bounds.max.z : bounds.min.z);
        const std::optional<glm::ivec2> pixel = projectToPixel(scene.camera, kShotSize, point);
        REQUIRE(pixel.has_value());
        min = glm::min(min, *pixel);
        max = glm::max(max, *pixel);
    }
    return {glm::max(min - kBloomMargin, glm::ivec2(0)), glm::min(max + kBloomMargin, kShotSize - 1)};
}

/// The pixels of `a` and `b` that differ, inside and outside `region`.
struct Changes {
    std::size_t inside = 0;
    std::size_t outside = 0;
    int largestOutside = 0;
};

Changes changesAround(const rv::assets::ImageData& a, const rv::assets::ImageData& b,
                      const std::pair<glm::ivec2, glm::ivec2>& region) {
    Changes changes;
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.width) + static_cast<std::size_t>(x)) *
                static_cast<std::size_t>(a.channels);
            int largest = 0;
            for (int channel = 0; channel < 3; ++channel) {
                largest =
                    std::max(largest, std::abs(static_cast<int>(a.pixels[at + static_cast<std::size_t>(channel)]) -
                                               static_cast<int>(b.pixels[at + static_cast<std::size_t>(channel)])));
            }
            const bool inRegion =
                x >= region.first.x && y >= region.first.y && x <= region.second.x && y <= region.second.y;
            if (!inRegion) {
                changes.largestOutside = std::max(changes.largestOutside, largest);
            }
            if (largest <= kPixelThreshold) {
                continue;
            }
            ++(inRegion ? changes.inside : changes.outside);
        }
    }
    return changes;
}

/// The lines of the run's log holding `needle` at warning level.
std::size_t warningsWith(const AppRun& run, std::string_view needle) {
    auto text = rv::core::readTextFile(run.outputFile);
    REQUIRE(text.has_value());
    std::size_t count = 0;
    for (const std::string_view line : rv::core::split(*text, '\n')) {
        if (line.find(needle) != std::string_view::npos && line.find("warn") != std::string_view::npos) {
            ++count;
        }
    }
    return count;
}

} // namespace

TEST_CASE("world: the stored room shows at the first frame before the snapshot, then is reconciled with nothing moved",
          "[gpu][world][world_start]") {
    ImportedRoom room("unchanged");
    // A first run makes the plan and keeps the room's items for the next start (no preview: nothing was stored).
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments("2"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    INFO(first.describe());
    CHECK(first.report->world.outcome == "generated");
    CHECK_FALSE(first.report->world.preview.has_value());
    CHECK_FALSE(first.report->world.reconcile.has_value());
    CHECK(first.report->world.itemsWritten);
    CHECK(first.report->world.screenGames == 53);
    REQUIRE(fs::exists(room.itemsFile()));
    const std::string items = rv::core::readTextFile(room.itemsFile()).value();
    const fs::path planFile = rv::world::worldFiles(room.data, rv::catalog::LibraryId::parse("esde").value()).plan;
    const std::string plan = rv::core::readTextFile(planFile).value();

    // One frame: the stored plan's room alone, whole (every layer from the cache), the CRT's colour bars.
    const fs::path previewShot = room.dir / "preview.png";
    const AppRun preview = runApp(retroverseExe(), room.dir, "preview", roomArguments("1", previewShot));
    requireCleanRun(preview);
    {
        INFO(preview.describe());
        const rv::app::WorldRecord& world = preview.report->world;
        CHECK(world.state == "room");
        CHECK(world.items == 50);
        CHECK(world.firstFrame == std::optional<std::uint64_t>(0));
        REQUIRE(world.preview.has_value());
        CHECK(world.preview->frame == 0);
        CHECK(world.preview->items == 50);
        CHECK(world.preview->itemsKnown == 50);
        CHECK(world.preview->frontsResident == std::optional<std::uint64_t>(50));
        CHECK(world.preview->spinesResident == std::optional<std::uint64_t>(50));
        CHECK(world.preview->planDigest == first.report->world.planDigest);
        REQUIRE(world.preview->committedAtMs.has_value());
        // The snapshot is asked for only once the stored room shows: it answered after.
        if (world.snapshotAtMs) {
            CHECK(*world.snapshotAtMs > *world.preview->committedAtMs);
        }
        CHECK_FALSE(world.reconcile.has_value()); // the run ended at the first frame
        CHECK(preview.report->video.picture == "idle-pattern");
    }

    // Forty frames: the same, then reconciled with the catalog unchanged: nothing moved, rebuilt or written; the CRT
    // plays the start choice.
    const fs::path reconciledShot = room.dir / "reconciled.png";
    const AppRun reconciled = runApp(retroverseExe(), room.dir, "reconciled", roomArguments("40", reconciledShot));
    requireCleanRun(reconciled);
    INFO(reconciled.describe());
    const rv::app::WorldRecord& world = reconciled.report->world;
    CHECK(world.state == "room");
    CHECK(world.outcome == "unchanged");
    CHECK_FALSE(world.planWritten);
    CHECK_FALSE(world.itemsWritten);
    CHECK(world.planDigest == first.report->world.planDigest);
    REQUIRE(world.preview.has_value());
    CHECK(world.preview->frame == 0);
    REQUIRE(world.reconcile.has_value());
    CHECK(world.reconcile->kind == "textures");
    CHECK(world.reconcile->frame == 1);
    CHECK(world.reconcile->kept == 50);
    CHECK(world.reconcile->moved == 0);
    CHECK(world.reconcile->flagsChanged == 0);
    CHECK(world.reconcile->added == 0);
    CHECK(world.reconcile->removed == 0);
    CHECK(world.reconcile->coversReloaded); // the media root known: the covers' sources checked
    REQUIRE(world.snapshotAtMs.has_value());
    CHECK(*world.snapshotAtMs > *world.preview->committedAtMs);
    CHECK(world.items == 50);
    CHECK(world.staticRecords == 150);
    CHECK(world.screenGames == 53);
    CHECK(reconciled.report->residency.frontsResident == 50);
    CHECK(reconciled.report->residency.spinesResident == 50);
    CHECK(reconciled.report->residency.cacheStored == 0);
    // Nothing written: the plan and the room's items as they were.
    CHECK(rv::core::readTextFile(planFile).value() == plan);
    CHECK(rv::core::readTextFile(room.itemsFile()).value() == items);
    // The CRT: the start choice, Amber Lantern's clip (the first featured title with a video), one decoder.
    const rv::app::VideoRecord& video = reconciled.report->video;
    REQUIRE(video.bound);
    CHECK(video.gameId == room.gameOf("Amber Lantern"));
    CHECK(video.picture == "video");
    CHECK(video.selections == 0);
    CHECK(video.decodersStarted == 1);
    CHECK(video.maxDecoderThreads == 1);
    CHECK(video.clipFramesUploaded >= 10);
    CHECK(video.fallbacks.empty());

    // The first frame showed the room the snapshot left as it was: the two pictures differ on the CRT alone.
    const rv::assets::ImageData before = requireScreenshot(preview, previewShot, kShotSize);
    const rv::assets::ImageData after = requireScreenshot(reconciled, reconciledShot, kShotSize);
    const auto region = crtRegion(shownScene(room, *reconciled.report));
    const Changes changes = changesAround(before, after, region);
    INFO("CRT region (" << region.first.x << ", " << region.first.y << ") to (" << region.second.x << ", "
                        << region.second.y << "): " << changes.inside << " pixel(s) changed inside, " << changes.outside
                        << " outside (largest change outside " << changes.largestOutside << ")");
    CHECK(changes.inside >= 100);
    CHECK(changes.outside <= changes.inside / 100);
}

TEST_CASE("world: a game added between two starts: the stored room first, then rebuilt around it, the walker kept",
          "[gpu][world][world_start]") {
    ImportedRoom room("added");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments("2"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    // One of G10's additions, its ROM and its cover, imported.
    for (const std::string_view file :
         {"roms/snes/Zinnia Zephyr (USA).sfc", "media/snes/covers/Zinnia Zephyr (USA).png"}) {
        std::error_code error;
        fs::copy_file(room.fixture.path(std::string("additions/") + std::string(file)), room.fixture.path(file), error);
        INFO(file << ": " << error.message());
        REQUIRE_FALSE(error);
    }
    room.cli("import_again", {"import"});
    // The walker steps forward on frames 0 to 5: the room committed again after frame 0 keeps it where it stands.
    const fs::path script = writeScript(
        room.dir, "walk.json", Json::array({{{"fromFrame", 0}, {"toFrame", 5}, {"actions", {{"move_forward", 1.0}}}}}));
    const AppRun added =
        runApp(retroverseExe(), room.dir, "added", roomArguments("10", std::nullopt, {"--input-script", arg(script)}));
    requireCleanRun(added);
    {
        INFO(added.describe());
        const rv::app::WorldRecord& world = added.report->world;
        REQUIRE(world.preview.has_value());
        CHECK(world.preview->frame == 0);
        CHECK(world.preview->items == 50);
        REQUIRE(world.reconcile.has_value());
        CHECK(world.reconcile->kind == "rebuild");
        CHECK(world.reconcile->frame == 1);
        CHECK(world.reconcile->kept == 50);
        CHECK(world.reconcile->moved == 0);
        CHECK(world.reconcile->added == 1);
        CHECK(world.reconcile->removed == 0);
        CHECK(world.outcome == "updated");
        CHECK(world.planWritten);
        CHECK(world.itemsWritten);
        CHECK(world.items == 51);
        CHECK(world.spine == 1);
        CHECK(world.staticRecords == 153);
        CHECK(added.report->residency.frontsResident == 51);
    }
    // The same walk on the unchanged catalog (nothing rebuilt) ends where the rebuilt room's did.
    const AppRun again =
        runApp(retroverseExe(), room.dir, "again", roomArguments("10", std::nullopt, {"--input-script", arg(script)}));
    requireCleanRun(again);
    INFO(again.describe());
    REQUIRE(again.report->world.reconcile.has_value());
    CHECK(again.report->world.reconcile->kind == "textures");
    CHECK(again.report->world.outcome == "unchanged");
    REQUIRE(added.report->finalCamera.has_value());
    REQUIRE(again.report->finalCamera.has_value());
    CHECK(glm::length(added.report->finalCamera->feet - again.report->finalCamera->feet) < 1e-4f);
    CHECK(glm::length(added.report->finalCamera->feet - first.report->finalCamera.value().feet) > 0.05f);
}

TEST_CASE("world: the room's CRT plays the quick catalog's selection, damaged clips falling back down the chain",
          "[gpu][world][world_start]") {
    ImportedRoom room("crt");
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments("2"));
    skipWithoutOpenGl(first);
    requireCleanRun(first);
    // A clip the catalog recorded that is gone since (ADR-0012 §7's chain: the file missing).
    REQUIRE(fs::remove(room.fixture.path("media/snes/videos/Ember Kettle Expedition (USA).mp4")));

    struct Case {
        std::string_view name;
        std::string_view search;
        std::string_view title;
        std::string_view picture;
        std::string_view reason; ///< the video's fallback reason ("" for none)
    };
    const std::vector<Case> cases{
        {"corrupt", "lilac loom", "Lilac Loom", "cover", "corrupt-data"},
        {"empty", "nutmeg nook", "Nutmeg Nook", "cover", "invalid-file"},
        {"unsupported_codec", "opal oriel", "Opal Oriel", "cover", "unsupported-codec"},
        {"missing", "ember kettle", "Ember Kettle Expedition", "cover", "file-not-found"},
        {"clip", "juniper jamboree", "Juniper Jamboree", "video", ""},
    };
    for (const Case& c : cases) {
        INFO(c.name);
        // The quick catalog opens on frame 2; ui_up on frames 4 and 6 takes the focus to the search field; the search
        // typed on frame 8 selects its first game, which the CRT plays after its dwell.
        const fs::path script =
            writeScript(room.dir, std::string(c.name) + ".json",
                        Json::array({{{"fromFrame", 2}, {"toFrame", 2}, {"actions", {{"quick_catalog", 1}}}},
                                     {{"fromFrame", 4}, {"toFrame", 4}, {"actions", {{"ui_up", 1}}}},
                                     {{"fromFrame", 6}, {"toFrame", 6}, {"actions", {{"ui_up", 1}}}},
                                     {{"fromFrame", 8}, {"toFrame", 8}, {"text", std::string(c.search)}}}));
        // 200 frames: the corrupt clip's damage lies between 40 and 80% of its file (media::writeCorruptedCopy), about
        // 1.6 s into it.
        const AppRun run = runApp(retroverseExe(), room.dir, c.name,
                                  roomArguments("200", std::nullopt, {"--input-script", arg(script)}));
        requireCleanRun(run);
        INFO(run.describe());
        REQUIRE(run.report->catalog.has_value());
        const std::string game = room.gameOf(c.title);
        CHECK(run.report->catalog->selectedId == game);
        const rv::app::VideoRecord& video = run.report->video;
        REQUIRE(video.bound);
        CHECK(video.gameId == game);
        CHECK(video.selections >= 1);
        CHECK(video.picture == c.picture);
        CHECK(video.maxDecoderThreads == 1);
        bool fellBack = false;
        for (const rv::app::VideoFallbackRecord& fallback : video.fallbacks) {
            if (fallback.gameId == game && fallback.picture == "video") {
                CHECK(fallback.reason == c.reason);
                fellBack = true;
            }
        }
        CHECK(fellBack == !c.reason.empty());
    }
}

TEST_CASE("world: a manifest removed from a scratch asset root falls back to its family's prefab, warned once",
          "[gpu][world][world_start]") {
    ImportedRoom room("object_fallback");
    // Style §26.3's "an asset lost on purpose": the hero-wall module's manifest removed from a copy of the build's
    // assets.
    const fs::path assets = room.dir / "assets";
    const fs::path source =
        buildConfig().devAssetRoot ? sourceDir() / "assets" : retroverseExe().parent_path() / "assets";
    fs::copy(source, assets, fs::copy_options::recursive);
    REQUIRE(fs::remove(rv::assets::prefabManifestPath(assets, "furniture.shelf.hero_module.generic")));
    const std::string warning = "Prefab 'furniture.shelf.hero_module.generic' cannot be used";
    for (const std::string_view name : {"generated", "restarted"}) {
        INFO(name);
        const fs::path shot = room.dir / (std::string(name) + ".png");
        const AppRun run =
            runApp(retroverseExe(), room.dir, name, roomArguments("5", shot, {"--asset-root", arg(assets)}));
        skipWithoutOpenGl(run);
        requireCleanRun(run, assets);
        INFO(run.describe());
        const rv::app::WorldRecord& world = run.report->world;
        CHECK(world.state == "room");
        CHECK(world.hardFindings == 0);
        CHECK(world.items == 50);
        CHECK(world.skipped == 0);
        // The side-wall module in the hero wall's sockets: no hero band, the six titles past the displays spine-out.
        CHECK(world.featured == 44);
        CHECK(world.band == 0);
        CHECK(world.spine == 6);
        REQUIRE(world.objectFallbacks.size() == 1);
        CHECK(world.objectFallbacks.front().prefab == "furniture.shelf.hero_module.generic");
        CHECK(world.objectFallbacks.front().source == "family generic");
        CHECK(world.objectFallbacks.front().reason == "the manifest is missing");
        // One warning per run, though the room's capacity, its scene and the restart's preview resolve it.
        CHECK(warningsWith(run, warning) == 1);
        CHECK(run.report->residency.frontsResident == 50);
        requireScreenshot(run, shot, kShotSize);
    }
}
