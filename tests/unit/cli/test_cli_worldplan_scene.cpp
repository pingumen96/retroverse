// retroverse-cli worldplan show --scene (I3 step 10a): the static part of a plan's room written as a scene file equal to
// world::toSceneFile's and loadable as a scene; the plan of the snes-room fixture's 50 titles (imported, generated,
// shown); failures. With RV_WORLD_SCENE_OUT naming a folder, the snes-room case also leaves its plan and scene file
// there for a manual look (retroverse.exe --scene <file>).

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

rv::core::PathEnvironment testEnvironment(const std::filesystem::path& executableDir) {
    rv::core::PathEnvironment environment;
    environment.executableDir = executableDir;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return environment;
}

/// A run of the dispatcher logs to stderr; this puts back the plain console logger afterwards.
class LoggingReset {
public:
    LoggingReset() = default;
    ~LoggingReset() {
        rv::core::LogConfig config;
        config.console = false;
        rv::core::initLogging(config);
        rv::core::shutdownLogging();
    }
    LoggingReset(const LoggingReset&) = delete;
    LoggingReset& operator=(const LoggingReset&) = delete;
};

struct RunResult {
    ExitCode code = ExitCode::Ok;
    std::string out;
    std::string err;
};

RunResult runCli(const std::vector<std::string>& arguments, const rv::core::PathEnvironment& environment) {
    Output out = Output::memory();
    Output err = Output::memory();
    const ExitCode code = rv::cli::run(arguments, out, err, environment);
    return RunResult{code, out.text(), err.text()};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::string readText(const std::filesystem::path& path) {
    auto text = rv::core::readTextFile(path);
    REQUIRE(text.has_value());
    return *text;
}

/// world::toSceneFile's text for the plan in `planPath`.
std::string expectedScene(const std::filesystem::path& planPath) {
    auto plan = rv::world::readPlan(readText(planPath), "plan");
    REQUIRE(plan.has_value());
    const rv::world::RoomId room = plan->rooms.front().id;
    auto data = rv::world::loadSceneData(rv::core::pathFromUtf8(RV_TEST_ASSET_DIR), *plan, room);
    REQUIRE(data.has_value());
    auto build = rv::world::toSceneFile(*plan, room, *data);
    REQUIRE(build.has_value());
    return rv::world::sceneFileText(build->sceneFile);
}

} // namespace

TEST_CASE("cli/worldplan show --scene: a golden plan's room as a scene file", "[cli][worldplan]") {
    const rv::test::TempDir dir("rv-cli-worldplan-scene");
    const LoggingReset loggingReset;
    const std::string data = rv::core::pathToUtf8(dir.file("data"));
    const rv::core::PathEnvironment environment = testEnvironment(dir.path());
    const std::filesystem::path plan = rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" /
                                       "golden" / "plan-890-seed-102938.json";
    const std::filesystem::path scene = dir.file("room.scene.json");

    const RunResult shown = runCli({"--data-dir", data, "worldplan", "show", rv::core::pathToUtf8(plan), "--scene",
                                    rv::core::pathToUtf8(scene)},
                                   environment);
    INFO(shown.err);
    REQUIRE(shown.code == ExitCode::Ok);
    CHECK(contains(shown.out, "Scene "));
    CHECK(contains(shown.out, " lights, 0 dressed span(s) left bare"));
    CHECK(readText(scene) == expectedScene(plan));
    auto loaded = rv::scene::loadSceneDescription(scene);
    INFO((loaded ? std::string("loaded") : loaded.error().toString()));
    REQUIRE(loaded.has_value());
    CHECK(loaded->name == "room:snes");
    CHECK(loaded->findObject("fx:hw-w") != nullptr);

    // --json names the scene file.
    const RunResult json = runCli({"--data-dir", data, "worldplan", "show", rv::core::pathToUtf8(plan), "--scene",
                                   rv::core::pathToUtf8(scene), "--json"},
                                  environment);
    REQUIRE(json.code == ExitCode::Ok);
    const nlohmann::json summary = nlohmann::json::parse(json.out);
    CHECK(summary.value("scene", std::string()) == rv::core::pathToUtf8(scene));

    // A scene that cannot be written (its folder is a file): exit 3.
    REQUIRE(rv::core::writeTextFileAtomic(dir.file("blocker"), "x").has_value());
    const RunResult unwritable = runCli({"--data-dir", data, "worldplan", "show", rv::core::pathToUtf8(plan), "--scene",
                                         rv::core::pathToUtf8(dir.file("blocker") / "room.json")},
                                        environment);
    CHECK(unwritable.code == ExitCode::RuntimeFailure);
}

TEST_CASE("cli/worldplan show --scene: the snes-room fixture's plan as a scene", "[cli][worldplan]") {
    auto fixture = rv::test::Fixture::materialize("esde/snes-room");
    INFO((fixture ? "materialized" : fixture.error().toString()));
    REQUIRE(fixture.has_value());
    const rv::test::TempDir dir("rv-cli-worldplan-scene-room");
    const LoggingReset loggingReset;
    const std::string data = rv::core::pathToUtf8(dir.file("data"));
    const rv::core::PathEnvironment environment = testEnvironment(dir.path());
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", data});
        return runCli(arguments, environment);
    };
    const RunResult created =
        run({"library", "create", "--home", rv::core::pathToUtf8(fixture->root().lexically_normal()), "--install",
             rv::core::pathToUtf8(fixture->path("Programs/ES-DE").lexically_normal())});
    INFO(created.err);
    REQUIRE(created.code == ExitCode::Ok);
    REQUIRE(run({"import"}).code == ExitCode::Ok);
    const RunResult generated = run({"worldplan", "generate", "--system", "snes", "--seed", "102938"});
    INFO(generated.err);
    REQUIRE(generated.code == ExitCode::Ok);

    const std::filesystem::path planPath = dir.file("data") / "worlds" / "esde" / "world-plan.json";
    const std::filesystem::path scene = dir.file("snes-room.scene.json");
    const RunResult shown = run({"worldplan", "show", "--scene", rv::core::pathToUtf8(scene)});
    INFO(shown.err);
    REQUIRE(shown.code == ExitCode::Ok);
    CHECK(readText(scene) == expectedScene(planPath));
    auto loaded = rv::scene::loadSceneDescription(scene);
    INFO((loaded ? std::string("loaded") : loaded.error().toString()));
    REQUIRE(loaded.has_value());
    // The fixture's 50 titles all stand face-out: no side-wall module, the two hero-wall modules dressed.
    auto plan = rv::world::readPlan(readText(planPath), "plan");
    REQUIRE(plan.has_value());
    CHECK(plan->placements.size() == 50);
    for (const rv::scene::SceneObject& object : loaded->objects) {
        const bool sideWall = object.id.rfind("fx:w", 0) == 0 || object.id.rfind("fx:e", 0) == 0;
        CHECK_FALSE((sideWall && object.kind == "shelf"));
    }
    CHECK(loaded->findObject("dress:fx:hw-w/l:0") != nullptr);

    if (const char* out = std::getenv("RV_WORLD_SCENE_OUT"); out != nullptr && *out != '\0') {
        const std::filesystem::path folder = rv::core::pathFromUtf8(out);
        std::filesystem::create_directories(folder);
        REQUIRE(rv::core::writeTextFileAtomic(folder / "snes-room.world-plan.json", readText(planPath)).has_value());
        REQUIRE(rv::core::writeTextFileAtomic(folder / "snes-room.scene.json", readText(scene)).has_value());
        WARN("wrote the snes-room plan and scene under " << out);
    }
}
