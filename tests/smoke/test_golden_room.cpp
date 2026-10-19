// The Gate B goldens of the generated room (I3 step 17a; style §26.1, docs/roadmap.md I3 row 17a): G02 (the snes-room
// fixture's 50 titles, all face-out), G02s (the snes-stress room: spine-out shelves, the hero band, a display case), G06
// (snes-single: one ROM, no media), G07 (an inspected cartridge with its card), G08a (the hero CRT's clip frozen at
// 1.0 s), G08b (the corrupt clip's fallback) and G10 (G02 after the fixture's 10 additions). Each is made from a fixture
// (tests/fixtures/esde/<name>: invented titles, generated covers and clips; never the user's media) recorded as the
// library of the case's data directory and imported with retroverse-cli, then retroverse.exe without --scene, hidden,
// lockstep (--fixed-dt: the first frame shows the room whole, its covers resident), at 1920x1080. Every run but G08b's
// holds the hero CRT's clip at 1.0 s (--video-freeze-pts), so that the screen is the same frame in every run.
//
// Each golden is assets/style_tests/screenshots/<stem>.png with <stem>.json beside it, as G01's: the GPU and driver,
// the commit, the SHA-256 of the image and of its inputs (the fixture's tree, the plan the run showed, the input
// script, the room's data under assets/world) and the commands of the run. The comparison is G01's
// (retroverse-screenshot-diff's tolerance: 6/255 per channel, at most 0.5% of the pixels) and runs on the reference
// machine only: another GL_RENDERER or GL_VERSION skips it. RV_UPDATE_GOLDEN=1 rewrites the image and its JSON from
// this machine's run and fails the case (a rewrite is never a pass: run again without it):
//   $env:RV_UPDATE_GOLDEN=1; build/msvc-x64-debug/bin/rv_smoke_tests.exe "golden/G02:*"; Remove-Item Env:RV_UPDATE_GOLDEN
// G10 is also compared with G02's golden: the pixels that differ lie where the ten added items stand.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/app/world_focus.hpp>
#include <rv/assets/image_io.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/math.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/screenshot_diff/screenshot_diff.hpp>
#include <rv/world/collection_layout.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/world_store.hpp>

#include "image_checks.hpp"
#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr glm::ivec2 kShotSize{1920, 1080};
constexpr float kEyeHeight = 1.65f;
/// The hero CRT's clip held at this time (G08a's frame) in every golden but G08b's.
constexpr std::string_view kFreezePts = "1";

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

fs::path goldenDir() {
    return sourceDir() / "assets/style_tests/screenshots";
}

bool updatingGoldens() {
    const char* value = std::getenv("RV_UPDATE_GOLDEN"); // NOLINT(concurrency-mt-unsafe): read before any thread
    return value != nullptr && std::string_view(value) == "1";
}

void replaceAll(std::string& text, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return;
    }
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
}

std::string fileSha256(const fs::path& file) {
    auto bytes = rv::core::readBinaryFile(file);
    REQUIRE(bytes.has_value());
    return rv::core::sha256Hex(std::span<const std::byte>(*bytes));
}

/// SHA-256 of a tree: one line "<relative path>\t<SHA-256 of the file>\n" per file, sorted by path (generic form).
std::string treeSha256(const fs::path& root) {
    std::vector<std::pair<std::string, fs::path>> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::string relative = rv::core::pathToUtf8(fs::relative(entry.path(), root));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            files.emplace_back(std::move(relative), entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::string lines;
    for (const auto& [relative, file] : files) {
        lines += relative + "\t" + fileSha256(file) + "\n";
    }
    return rv::core::sha256Hex(std::string_view(lines));
}

/// The commit checked out in the source tree (a worktree's .git file followed), "" when it cannot be read.
std::string gitHead() {
    fs::path gitDir = sourceDir() / ".git";
    if (fs::is_regular_file(gitDir)) {
        auto text = rv::core::readTextFile(gitDir);
        if (!text || !text->starts_with("gitdir:")) {
            return {};
        }
        const std::string target(rv::core::trim(std::string_view(*text).substr(7)));
        gitDir = rv::core::pathFromUtf8(target);
        if (gitDir.is_relative()) {
            gitDir = sourceDir() / gitDir;
        }
    }
    fs::path commonDir = gitDir;
    if (auto common = rv::core::readTextFile(gitDir / "commondir")) {
        commonDir = gitDir / rv::core::pathFromUtf8(std::string(rv::core::trim(*common)));
    }
    auto head = rv::core::readTextFile(gitDir / "HEAD");
    if (!head) {
        return {};
    }
    const std::string headText(rv::core::trim(*head));
    if (!headText.starts_with("ref:")) {
        return headText;
    }
    const std::string ref(rv::core::trim(std::string_view(headText).substr(4)));
    for (const fs::path& dir : {gitDir, commonDir}) {
        if (auto value = rv::core::readTextFile(dir / rv::core::pathFromUtf8(ref))) {
            return std::string(rv::core::trim(*value));
        }
    }
    if (auto packed = rv::core::readTextFile(commonDir / "packed-refs")) {
        std::string_view rest(*packed);
        while (!rest.empty()) {
            const std::size_t end = rest.find('\n');
            const std::string_view line = rv::core::trim(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view() : rest.substr(end + 1);
            if (line.size() > 41 && line.substr(41) == ref) {
                return std::string(line.substr(0, 40));
            }
        }
    }
    return {};
}

/// A fixture recorded as the library of <caseDir>/data and imported with retroverse-cli; every command the golden's
/// runs take recorded, with the case's and the fixture's folders as <case> and <fixture>.
struct GoldenRoom {
    std::string fixtureName;
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;
    Json steps = Json::array();

    GoldenRoom(std::string_view name, std::string_view fixtureName_)
        : fixtureName(fixtureName_), fixture(rv::test::Fixture::materialize(fixtureName_).value()),
          dir(freshCaseDir(std::string("golden_room/") + std::string(name))), data(dir / "data") {
        steps.push_back("materialize tests/fixtures/" + fixtureName + " into <fixture> (rv::test::Fixture)");
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    /// `text` with this machine's folders written as <case>, <fixture> and paths relative to the source tree.
    [[nodiscard]] std::string portable(std::string text) const {
        const auto slashed = [](std::string path) {
            std::replace(path.begin(), path.end(), '\\', '/');
            return path;
        };
        text = slashed(std::move(text));
        replaceAll(text, slashed(arg(dir)), "<case>");
        replaceAll(text, slashed(arg(fixture.root())), "<fixture>");
        replaceAll(text, slashed(arg(sourceDir())) + "/", "");
        return text;
    }

    [[nodiscard]] std::string commandLine(const fs::path& exe, const std::vector<std::string>& arguments) const {
        std::string line = arg(exe);
        for (const std::string& argument : arguments) {
            line += argument.find(' ') == std::string::npos ? " " + argument : " \"" + argument + "\"";
        }
        return portable(line);
    }

    void cli(std::string_view name, std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        steps.push_back(commandLine(retroverseCliExe(), arguments));
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    /// runApp() of retroverse.exe, recorded with the options runApp() adds (but the run report).
    AppRun app(std::string_view name, const std::vector<std::string>& arguments) {
        std::vector<std::string> recorded = arguments;
        recorded.insert(recorded.end(), {"--hidden", "--data-dir", arg(data)});
        steps.push_back(commandLine(retroverseExe(), recorded));
        return runApp(retroverseExe(), dir, name, arguments);
    }
};

/// A lockstep run of the room: `frames` frames at a fixed step, a screenshot at 1920x1080 at the end.
std::vector<std::string> roomArguments(std::uint32_t frames, const fs::path& shot, std::vector<std::string> extra = {}) {
    std::vector<std::string> arguments{"--exit-after-frames", "--frames",           std::to_string(frames),
                                       "--fixed-dt",          "0.016667",           "--screenshot",
                                       arg(shot),             "--screenshot-size", "1920x1080"};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return arguments;
}

/// The plan the run showed and the room's scene and collection, as retroverse.exe made them from it.
struct ShownRoom {
    fs::path planFile;
    rv::world::StoredPlan stored;
    rv::world::SceneData data;
    rv::scene::SceneDescription scene;
    rv::world::CollectionLayout layout;
};

ShownRoom shownRoom(const GoldenRoom& room, const rv::app::RunReport& report) {
    const fs::path planFile =
        rv::world::worldFiles(room.data, rv::catalog::LibraryId::parse(report.world.library).value()).plan;
    auto stored = rv::world::readStoredPlan(planFile);
    REQUIRE(stored.has_value());
    const rv::world::RoomId roomId = rv::world::RoomId::parse(report.world.room).value();
    auto data = rv::world::loadSceneData(report.assetRoot, stored->plan, roomId);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneDescription(stored->plan, roomId, *data);
    REQUIRE(scene.has_value());
    auto layout = rv::world::collectionLayout(stored->plan, roomId, *data, {});
    REQUIRE(layout.has_value());
    return ShownRoom{planFile, std::move(*stored), std::move(*data), std::move(*scene), std::move(*layout)};
}

/// The checks of every run: clean, the generated room shown.
void requireRoomRun(const AppRun& run) {
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.bound);
    REQUIRE(run.report->world.state == "room");
    CHECK(run.report->world.hardFindings == 0);
}

/// The checks of a golden's run: the stored plan's room shown unchanged at the first frame (the lockstep kept: no wait
/// for the room ran out), every item in view resident when the screenshot was taken.
void requireGoldenRun(const AppRun& run) {
    requireRoomRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.firstFrame.has_value());
    CHECK(*run.report->world.firstFrame == 0);
    CHECK(run.report->residency.viewResidentAfterShownMs.has_value());
}

/// The first run of a golden's room: the plan generated and written, the covers made into the derived-texture cache.
/// 300 frames, so that the room comes even when a loaded machine makes the first frame's wait for it (10 s) run out:
/// the golden's own run then starts from the stored plan, like every later start of the app.
rv::app::RunReport firstRun(GoldenRoom& room) {
    const AppRun first = runApp(retroverseExe(), room.dir, "first", roomArguments(300, room.dir / "first.png"));
    skipWithoutOpenGl(first);
    requireRoomRun(first);
    INFO(first.describe());
    CHECK(first.report->world.outcome == "generated");
    CHECK(first.report->world.planWritten);
    room.steps.push_back("a first run of the same command but --frames 300 and without --video-freeze-pts, "
                         "--spawn or --input-script: the plan generated and written, the covers into the cache");
    return *first.report;
}

const rv::world::CollectionItem& itemIn(const ShownRoom& room, std::string_view slot) {
    const auto found = std::find_if(room.layout.items.begin(), room.layout.items.end(), [&](const auto& item) {
        return item.slot.toString().ends_with("/" + std::string(slot));
    });
    INFO("the item in " << slot);
    REQUIRE(found != room.layout.items.end());
    return *found;
}

/// The walker at `item`'s Focus pose as --spawn takes it (the default window's aspect).
rv::app::SpawnPoint focusSpawn(const rv::world::CollectionItem& item, const rv::scene::CameraSetup& lens) {
    rv::app::FocusViewer viewer;
    viewer.lens = lens.lens;
    viewer.aspect = 1280.0f / 720.0f;
    const rv::scene::WalkerPose pose = rv::app::focusPoseFor(item, viewer);
    rv::app::SpawnPoint spawn;
    spawn.feet = pose.feet;
    spawn.yawDeg = rv::core::radiansToDegrees(pose.yaw);
    spawn.pitchDeg = rv::core::radiansToDegrees(pose.pitch);
    return spawn;
}

/// The walker standing `distance` in front of the hero CRT's screen (its ScreenSurface socket), looking at its middle.
rv::app::SpawnPoint crtSpawn(const ShownRoom& shown, float distance) {
    const rv::world::Fixture* crt = nullptr;
    for (const rv::world::Fixture& fixture : shown.stored.plan.fixtures) {
        if (fixture.role == rv::world::FixtureRole::HeroCorner && fixture.id.part() == "crt") {
            crt = &fixture;
        }
    }
    REQUIRE(crt != nullptr);
    const rv::world::ScenePrefab* prefab = shown.data.prefabs.find(crt->prefab);
    REQUIRE(prefab != nullptr);
    REQUIRE(prefab->prefab.has_value());
    const rv::geometry::Socket* screen = nullptr;
    for (const rv::geometry::Socket& candidate : prefab->prefab->sockets) {
        if (candidate.id == "ScreenSurface") {
            screen = &candidate;
        }
    }
    REQUIRE(screen != nullptr);
    const glm::mat4 frame = rv::world::room_frames::fixtureTransform(*crt) * screen->transform;
    const glm::vec3 center(frame * glm::vec4(screen->bounds.center().x, screen->bounds.center().y, 0.0f, 1.0f));
    glm::vec3 out(frame * glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    out.y = 0.0f;
    out = glm::normalize(out);
    const glm::vec3 eye(center.x + distance * out.x, kEyeHeight, center.z + distance * out.z);
    const glm::vec3 look = center - eye;
    rv::app::SpawnPoint spawn;
    spawn.feet = glm::vec3(eye.x, 0.0f, eye.z);
    spawn.yawDeg = rv::core::radiansToDegrees(std::atan2(look.x, -look.z));
    spawn.pitchDeg = rv::core::radiansToDegrees(std::atan2(look.y, std::sqrt(look.x * look.x + look.z * look.z)));
    return spawn;
}

/// What a golden is and what its JSON records beside the run's commands.
struct GoldenSpec {
    std::string_view id;
    std::string_view stem;
    std::string_view description;
    std::string_view view;
    std::optional<std::string_view> inputScript;
};

std::string verdictLine(const std::string& text) {
    return text + "\n";
}

/// Compares the run's screenshot with the golden `spec` names, on the reference machine only (RV_UPDATE_GOLDEN=1:
/// rewrites the golden and its JSON, then fails; skipped on another GPU or driver).
void compareWithGolden(const GoldenSpec& spec, const GoldenRoom& room, const AppRun& run, const fs::path& shot,
                       std::uint32_t frames) {
    const fs::path metadataFile = goldenDir() / (std::string(spec.stem) + ".json");
    const fs::path golden = goldenDir() / (std::string(spec.stem) + ".png");
    const fs::path verdictFile = room.dir / "comparison.txt";
    const rv::app::RunReportContext& context = *run.report->context;
    const ShownRoom shown = shownRoom(room, *run.report);

    if (updatingGoldens()) {
        std::error_code error;
        fs::copy_file(shot, golden, fs::copy_options::overwrite_existing, error);
        INFO(error.message());
        REQUIRE_FALSE(error);
        Json inputs{{"fixture",
                     {{"name", room.fixtureName},
                      {"path", "tests/fixtures/" + room.fixtureName},
                      {"sha256", treeSha256(sourceDir() / "tests/fixtures" / rv::core::pathFromUtf8(room.fixtureName))},
                      {"rule", "SHA-256 of the lines \"<relative path>\\t<SHA-256 of the file>\\n\", sorted by path"}}},
                    {"plan",
                     {{"digest", run.report->world.planDigest},
                      {"fileSha256", fileSha256(shown.planFile)},
                      {"placements", run.report->world.placements},
                      {"items", run.report->world.items}}},
                    {"worldData", {{"path", "assets/world"}, {"sha256", treeSha256(sourceDir() / "assets/world")}}}};
        if (spec.inputScript) {
            const std::string script = "tests/input_scripts/" + std::string(*spec.inputScript);
            inputs["inputScript"] = {{"file", script}, {"sha256", fileSha256(sourceDir() / script)}};
        }
        const std::string preset = rv::core::pathToUtf8(retroverseExe().parent_path().parent_path().filename());
        const Json metadata{
            {"schemaVersion", 1},
            {"id", spec.id},
            {"description", spec.description},
            {"image",
             {{"file", std::string(spec.stem) + ".png"},
              {"width", kShotSize.x},
              {"height", kShotSize.y},
              {"format", "PNG, 8-bit RGB, sRGB-encoded as the window shows it"},
              {"sha256", fileSha256(golden)}}},
            {"inputs", inputs},
            {"view", spec.view},
            {"run",
             {{"frames", frames},
              {"capturedFrame", frames - 1},
              {"steps", room.steps},
              {"regenerate", "$env:RV_UPDATE_GOLDEN=1; build/" + preset + "/bin/rv_smoke_tests.exe \"golden/" +
                                 std::string(spec.id) + ":*\"; Remove-Item Env:RV_UPDATE_GOLDEN (then run it again "
                                 "without the variable; tests/smoke/test_golden_room.cpp)"}}},
            {"build",
             {{"preset", preset},
              {"buildType", run.report->buildType},
              {"gitCommit", gitHead()},
              {"sources", "gitCommit plus the changes of the commit that adds or rewrites this metadata, rendered from "
                          "that working tree before committing"}}},
            {"gpu",
             {{"vendor", context.vendor},
              {"renderer", context.renderer},
              {"glVersion", context.version},
              {"machine", "docs/testing/reference-hardware.md"}}},
            {"renderedOn", rv::core::utcNow().substr(0, 10)},
            {"comparison", "Compared by tests/smoke/test_golden_room.cpp with G01's tolerance (6/255 per channel, at "
                           "most 0.5% of the pixels) on this GPU and driver only."}};
        REQUIRE(rv::core::writeTextFileAtomic(metadataFile, metadata.dump(2) + "\n").has_value());
        REQUIRE(rv::core::writeTextFileAtomic(verdictFile, verdictLine("rewritten")).has_value());
        FAIL("golden " << spec.id << " rewritten from this run (" << rv::core::pathToUtf8(golden)
                       << "): look at it, then run again without RV_UPDATE_GOLDEN");
    }

    const auto metadata = rv::core::readJsonFile(metadataFile);
    INFO(rv::core::pathToUtf8(metadataFile));
    REQUIRE(metadata.has_value());
    const std::string renderer = metadata->at("gpu").at("renderer").get<std::string>();
    const std::string glVersion = metadata->at("gpu").at("glVersion").get<std::string>();
    if (context.renderer != renderer || context.version != glVersion) {
        const std::string reason = "golden " + std::string(spec.id) + " was rendered on " + renderer + " (" +
                                   glVersion + "), this machine has " + context.renderer + " (" + context.version +
                                   "): the comparison runs on the reference machine only "
                                   "(docs/testing/reference-hardware.md)";
        REQUIRE(rv::core::writeTextFileAtomic(verdictFile, verdictLine("skipped: " + reason)).has_value());
        SKIP(reason);
    }
    // A plan other than the golden's shows another room: said before the pixels are.
    const std::string recordedPlan = metadata->at("inputs").at("plan").at("digest").get<std::string>();
    if (recordedPlan != run.report->world.planDigest) {
        WARN("the plan shown (" << run.report->world.planDigest << ") is not the golden's (" << recordedPlan
                                << "): the generator, the template or the fixture changed");
    }

    const rv::screenshot_diff::Tolerance tolerance;
    const fs::path heatmap = room.dir / (std::string(spec.stem) + "_heatmap.png");
    const auto comparison = rv::screenshot_diff::compareFiles(golden, shot, tolerance, heatmap);
    REQUIRE(comparison.has_value());
    const std::string verdict = comparison->describe(tolerance);
    REQUIRE(rv::core::writeTextFileAtomic(verdictFile, verdictLine(verdict)).has_value());
    INFO(verdict);
    INFO("heatmap of the differences: " << rv::core::pathToUtf8(heatmap));
    CHECK(comparison->matches);
}

} // namespace

TEST_CASE("golden/G02: the snes-room's 50 titles, all face-out, from the doorway", "[gpu][golden][golden_room]") {
    GoldenRoom room("g02", "esde/snes-room");
    static_cast<void>(firstRun(room));
    const fs::path shot = room.dir / "g02.png";
    constexpr std::uint32_t kFrames = 30;
    const AppRun run = room.app("g02", roomArguments(kFrames, shot, {"--video-freeze-pts", std::string(kFreezePts)}));
    requireGoldenRun(run);
    INFO(run.describe());
    CHECK(run.report->world.outcome == "unchanged");
    CHECK(run.report->world.items == 50);
    CHECK(run.report->world.spine == 0);
    CHECK(run.report->residency.frontsResident == 50);
    requireScreenshot(run, shot, kShotSize);
    compareWithGolden({"G02", "g02_snes_room_golden",
                       "Gate B golden of the generated room (style 26.1 2): the snes-room fixture's 50 invented titles, "
                       "every one face-out in the display cases, the featured wall shelf and the hero band, from the "
                       "doorway (the plan's scene camera), the hero CRT's clip held at 1.0 s.",
                       "the plan's scene camera at the doorway (no --spawn)", std::nullopt},
                      room, run, shot, kFrames);
}

TEST_CASE("golden/G02s: the snes-stress room's spine-out shelves, the hero band and a display case",
          "[gpu][golden][golden_room]") {
    GoldenRoom room("g02s", "esde/snes-stress");
    static_cast<void>(firstRun(room));
    const fs::path shot = room.dir / "g02s.png";
    constexpr std::uint32_t kFrames = 30;
    const AppRun run = room.app("g02s", roomArguments(kFrames, shot, {"--video-freeze-pts", std::string(kFreezePts)}));
    requireGoldenRun(run);
    INFO(run.describe());
    CHECK(run.report->world.outcome == "unchanged");
    CHECK(run.report->world.spine > 1000);
    requireScreenshot(run, shot, kShotSize);
    compareWithGolden({"G02s", "g02s_snes_stress_golden",
                       "Gate B golden of the generated room at its largest (an addition to style 26.1's list): the "
                       "snes-stress fixture's 2,000 invented titles, the spine-out modules of the hero and side walls, "
                       "the hero band and the display cases, from the doorway (the plan's scene camera), the hero "
                       "CRT's clip held at 1.0 s.",
                       "the plan's scene camera at the doorway (no --spawn)", std::nullopt},
                      room, run, shot, kFrames);
}

TEST_CASE("golden/G06: the snes-single room, one ROM and no media", "[gpu][golden][golden_room]") {
    GoldenRoom room("g06", "esde/snes-single");
    static_cast<void>(firstRun(room));
    const fs::path shot = room.dir / "g06.png";
    constexpr std::uint32_t kFrames = 30;
    const AppRun run = room.app("g06", roomArguments(kFrames, shot, {"--video-freeze-pts", std::string(kFreezePts)}));
    requireGoldenRun(run);
    INFO(run.describe());
    CHECK(run.report->world.outcome == "unchanged");
    CHECK(run.report->world.items == 1);
    requireScreenshot(run, shot, kShotSize);
    compareWithGolden({"G06", "g06_snes_single_golden",
                       "Gate B golden of the P0 single-ROM room (style 26.1 6, style 21): the snes-single fixture's "
                       "one ROM without any media, its cartridge face-out with a generated label, the room furnished "
                       "from the template alone, the hero CRT without a clip, from the doorway (the plan's scene "
                       "camera).",
                       "the plan's scene camera at the doorway (no --spawn)", std::nullopt},
                      room, run, shot, kFrames);
}

TEST_CASE("golden/G07: an inspected cartridge with its card", "[gpu][golden][golden_room]") {
    GoldenRoom room("g07", "esde/snes-room");
    const ShownRoom shown = shownRoom(room, firstRun(room));
    const rv::world::CollectionItem& item = itemIn(shown, "fx:hw-w/s:5.4");
    const fs::path shot = room.dir / "g07.png";
    constexpr std::uint32_t kFrames = 90;
    const AppRun run = room.app(
        "g07", roomArguments(kFrames, shot,
                             {"--spawn", spawnArgument(focusSpawn(item, shown.scene.camera)), "--input-script",
                              arg(inputScript("golden_g07_inspect.json")), "--video-freeze-pts",
                              std::string(kFreezePts)}));
    requireGoldenRun(run);
    INFO(run.describe());
    CHECK(run.report->world.outcome == "unchanged");
    CHECK(run.report->world.interaction.state == "inspect");
    CHECK(run.report->world.interaction.inspect.lift == 1.0);
    CHECK(run.report->world.card.game == item.game.toString());
    CHECK(run.report->world.card.coverFrames > 0);
    requireScreenshot(run, shot, kShotSize);
    compareWithGolden({"G07", "g07_inspect_card_golden",
                       "Gate B golden of the inspection (style 26.1 7): the snes-room fixture's band item "
                       "fx:hw-w/s:5.4 held in the hand, turned 15 degrees, the scene behind dimmed, its card beside it "
                       "(the product UI, laid out for the 1280x720 window and drawn into the screenshot), the hero "
                       "CRT's clip held at 1.0 s.",
                       "--spawn at the Focus pose of fx:hw-w/s:5.4 (app::focusPoseFor, the 16:9 window), "
                       "tests/input_scripts/golden_g07_inspect.json",
                       "golden_g07_inspect.json"},
                      room, run, shot, kFrames);
}

TEST_CASE("golden/G08a: the hero CRT with its clip frozen at 1.0 s", "[gpu][golden][golden_room]") {
    GoldenRoom room("g08a", "esde/snes-room");
    const ShownRoom shown = shownRoom(room, firstRun(room));
    const fs::path shot = room.dir / "g08a.png";
    constexpr std::uint32_t kFrames = 30;
    const AppRun run =
        room.app("g08a", roomArguments(kFrames, shot,
                                       {"--spawn", spawnArgument(crtSpawn(shown, 1.6f)), "--video-freeze-pts",
                                        std::string(kFreezePts)}));
    requireGoldenRun(run);
    INFO(run.describe());
    const rv::app::VideoRecord& video = run.report->video;
    REQUIRE(video.bound);
    CHECK(video.picture == "video");
    REQUIRE(video.freezePtsSeconds.has_value());
    CHECK(*video.freezePtsSeconds == 1.0);
    CHECK(video.maxDecoderThreads <= 1);
    requireScreenshot(run, shot, kShotSize);
    compareWithGolden({"G08a", "g08a_crt_frozen_golden",
                       "Gate B golden of the hero CRT's video (style 26.1 8, a TV with its video): the snes-room "
                       "fixture's room, the walker 1.6 m in front of the CRT's screen, the start choice's generated "
                       "clip held at 1.0 s (--video-freeze-pts) on the tube, its body, bezel and spill on the cabinet.",
                       "--spawn 1.6 m in front of the CRT's ScreenSurface, at eye height, looking at its middle",
                       std::nullopt},
                      room, run, shot, kFrames);
}

TEST_CASE("golden/G08b: the hero CRT falls back from the corrupt clip to the game's cover",
          "[gpu][golden][golden_room]") {
    GoldenRoom room("g08b", "esde/snes-room");
    const ShownRoom shown = shownRoom(room, firstRun(room));
    const fs::path shot = room.dir / "g08b.png";
    constexpr std::uint32_t kFrames = 240;
    const AppRun run = room.app("g08b", roomArguments(kFrames, shot,
                                                      {"--spawn", spawnArgument(crtSpawn(shown, 1.6f)),
                                                       "--input-script",
                                                       arg(inputScript("golden_g08b_corrupt_clip.json"))}));
    requireGoldenRun(run);
    INFO(run.describe());
    REQUIRE(run.report->catalog.has_value());
    CHECK_FALSE(run.report->catalog->open);
    const rv::app::VideoRecord& video = run.report->video;
    REQUIRE(video.bound);
    CHECK(video.gameId == run.report->catalog->selectedId);
    CHECK(video.picture == "cover");
    bool corrupt = false;
    for (const rv::app::VideoFallbackRecord& fallback : video.fallbacks) {
        corrupt = corrupt || (fallback.gameId == video.gameId && fallback.picture == "video" &&
                              fallback.reason == "corrupt-data");
    }
    CHECK(corrupt);
    requireScreenshot(run, shot, kShotSize);
    compareWithGolden({"G08b", "g08b_crt_fallback_golden",
                       "Gate B golden of the hero CRT in fallback (style 26.1 8, a TV in fallback): G08a's view, Lilac "
                       "Loom selected in the quick catalog (then closed): its corrupt clip plays until the damage and "
                       "falls back down the chain to the game's cover, which the tube shows at the end.",
                       "G08a's --spawn, tests/input_scripts/golden_g08b_corrupt_clip.json",
                       "golden_g08b_corrupt_clip.json"},
                      room, run, shot, kFrames);
}

TEST_CASE("golden/G10: G02 after 10 games added, which stand where nothing stood", "[gpu][golden][golden_room]") {
    GoldenRoom room("g10", "esde/snes-room");
    const ShownRoom before = shownRoom(room, firstRun(room));
    // The fixture's additions (ten ROMs, their covers and gamelist entries) laid over the library, imported again.
    REQUIRE(room.fixture.applyOverlay("additions").has_value());
    room.steps.push_back("lay <fixture>/additions over <fixture> (rv::test::Fixture::applyOverlay)");
    room.cli("import_again", {"import"});
    const fs::path shot = room.dir / "g10.png";
    constexpr std::uint32_t kFrames = 30;
    const AppRun run = room.app("g10", roomArguments(kFrames, shot, {"--video-freeze-pts", std::string(kFreezePts)}));
    requireGoldenRun(run);
    INFO(run.describe());
    const rv::app::WorldRecord& world = run.report->world;
    CHECK(world.outcome == "updated");
    CHECK(world.items == 60);
    CHECK(world.added == 10);
    REQUIRE(world.reconcile.has_value());
    CHECK(world.reconcile->moved == 0);
    CHECK(world.reconcile->kept == 50);
    CHECK(run.report->residency.frontsResident == 60);
    const rv::assets::ImageData image = requireScreenshot(run, shot, kShotSize);
    compareWithGolden({"G10", "g10_snes_room_added_golden",
                       "Gate B golden of the incremental update (style 26.1 10): G02's room after the snes-room "
                       "fixture's ten additions were imported: the 50 items where they stood, the ten new titles in "
                       "the spine-out reserve, from the doorway (the plan's scene camera), the hero CRT's clip held at "
                       "1.0 s.",
                       "the plan's scene camera at the doorway (no --spawn), as G02's", std::nullopt},
                      room, run, shot, kFrames);

    // Against G02's golden: what differs lies where the ten added items stand: their level of their module, which they
    // took from its dressing (ADR-0010 §10: a full spine-out section takes a dressed level), as the camera sees it (the
    // items' boxes in height, the module's in width), grown by a margin for shadows, ambient occlusion and FXAA.
    const ShownRoom after = shownRoom(room, *run.report);
    std::set<std::string> earlier;
    for (const rv::world::CollectionItem& item : before.layout.items) {
        earlier.insert(item.game.toString());
    }
    REQUIRE(after.scene.camera.position == before.scene.camera.position);
    struct Rect {
        glm::ivec2 min{kShotSize};
        glm::ivec2 max{0};
        void add(const std::optional<glm::ivec2>& pixel) {
            if (pixel) {
                min = glm::min(min, *pixel);
                max = glm::max(max, *pixel + 1);
            }
        }
        [[nodiscard]] bool empty() const { return max.x <= min.x || max.y <= min.y; }
    };
    const auto project = [&](Rect& rect, const glm::mat4& frame, const rv::core::Aabb& box) {
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local((corner & 1) ? box.max.x : box.min.x, (corner & 2) ? box.max.y : box.min.y,
                                  (corner & 4) ? box.max.z : box.min.z);
            rect.add(projectToPixel(after.scene.camera, kShotSize, glm::vec3(frame * glm::vec4(local, 1.0f))));
        }
    };
    constexpr int kMargin = 24;
    std::vector<Rect> added;
    std::size_t addedItems = 0;
    for (const rv::world::CollectionItem& item : after.layout.items) {
        if (earlier.contains(item.game.toString())) {
            continue;
        }
        ++addedItems;
        Rect items;
        project(items, item.slotFrame, item.slotBox);
        Rect module;
        for (const rv::world::Fixture& fixture : after.stored.plan.fixtures) {
            if (fixture.id == item.slot.fixture()) {
                const rv::world::ScenePrefab* prefab = after.data.prefabs.find(fixture.prefab);
                REQUIRE(prefab != nullptr);
                REQUIRE(prefab->prefab.has_value());
                project(module, rv::world::room_frames::fixtureTransform(fixture), prefab->prefab->bounds);
            }
        }
        if (!items.empty()) {
            Rect level = items;
            if (!module.empty()) {
                level.min.x = std::min(level.min.x, module.min.x);
                level.max.x = std::max(level.max.x, module.max.x);
            }
            added.push_back({level.min - kMargin, level.max + kMargin});
        }
    }
    CHECK(addedItems == 10);
    CHECK_FALSE(added.empty());
    auto g02 = rv::assets::loadImage(goldenDir() / "g02_snes_room_golden.png", 3);
    REQUIRE(g02.has_value());
    REQUIRE(image.channels >= 3);
    std::size_t differing = 0;
    std::size_t outside = 0;
    for (int y = 0; y < kShotSize.y; ++y) {
        for (int x = 0; x < kShotSize.x; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(kShotSize.x) +
                                   static_cast<std::size_t>(x);
            int largest = 0;
            for (int c = 0; c < 3; ++c) {
                largest = std::max(largest, std::abs(static_cast<int>(g02->pixels[at * 3 + c]) -
                                                     static_cast<int>(image.pixels[at * image.channels + c])));
            }
            if (largest <= 6) {
                continue;
            }
            ++differing;
            const bool inside = std::any_of(added.begin(), added.end(), [&](const Rect& rect) {
                return x >= rect.min.x && x < rect.max.x && y >= rect.min.y && y < rect.max.y;
            });
            outside += inside ? 0 : 1;
        }
    }
    auto heatmap = rv::screenshot_diff::makeHeatmap(*g02, image, 6);
    REQUIRE(heatmap.has_value());
    const fs::path heatmapFile = room.dir / "g10_against_g02_heatmap.png";
    REQUIRE(rv::assets::writePng(heatmapFile, *heatmap).has_value());
    INFO(differing << " pixels differ from G02's golden by more than 6/255, " << outside
                   << " of them outside the level the ten added items took (heatmap " << rv::core::pathToUtf8(heatmapFile)
                   << ")");
    CHECK(differing > 0);
    CHECK(outside == 0);
}
