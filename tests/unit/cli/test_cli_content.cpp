// I11 step 7: retroverse-cli content (add-console, check, list) on a data directory of its own, with the fixture console
// made at test time.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/console_content.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

#include "support/fixture_console.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

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

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

class ContentCli {
public:
    ContentCli() : m_dir("rv-cli-content"), m_data(m_dir.file("data")) {}

    [[nodiscard]] RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(m_data)});
        Output out = Output::memory();
        Output err = Output::memory();
        rv::core::PathEnvironment environment;
        environment.executableDir = m_dir.path();
        environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
        const ExitCode code = rv::cli::run(arguments, out, err, environment);
        return RunResult{code, out.text(), err.text()};
    }

    [[nodiscard]] fs::path model(std::string_view name, const rv::test::FixtureConsoleOptions& options = {}) const {
        const fs::path file = m_dir.file(std::string(name));
        REQUIRE(rv::test::writeFixtureConsole(file, options).has_value());
        return file;
    }

    [[nodiscard]] fs::path consoles() const { return m_data / "content" / "consoles"; }

private:
    rv::test::TempDir m_dir;
    fs::path m_data;
};

std::vector<std::string> addArguments(const fs::path& model, std::string_view system) {
    return {"content", "add-console", "--system", std::string(system), "--model", rv::core::pathToUtf8(model),
            "--source", "https://example.invalid/console", "--author", "Fixture Works", "--license", "CC-BY-4.0",
            "--scale-cm", "24,7,20"};
}

} // namespace

TEST_CASE("cli/content: add-console copies a model that passes, with its console.json", "[cli][content]") {
    const LoggingReset logging;
    const ContentCli cli;
    const RunResult empty = cli.run({"content", "list"});
    CHECK(empty.code == ExitCode::Ok);
    CHECK(contains(empty.out, "No console models"));

    const RunResult added = cli.run(addArguments(cli.model("my console.glb"), "snes"));
    INFO(added.out << added.err);
    REQUIRE(added.code == ExitCode::Ok);
    CHECK(contains(added.out, "snes: ok (home): 24.0 x 7.0 x 20.0 cm"));
    CHECK(fs::exists(cli.consoles() / "snes" / "console.glb"));
    auto manifest = rv::assets::loadConsoleContent(cli.consoles() / "snes");
    REQUIRE(manifest.has_value());
    CHECK(manifest->manifest.source.author == "Fixture Works");
    CHECK(manifest->manifest.source.license == "CC-BY-4.0");
    CHECK(manifest->manifest.scaleCm == std::optional<glm::vec3>(glm::vec3(24.0F, 7.0F, 20.0F)));

    // A second one for the same system only with --replace.
    CHECK(cli.run(addArguments(cli.model("again.glb"), "snes")).code == ExitCode::UsageError);
    std::vector<std::string> replace = addArguments(cli.model("again.glb"), "snes");
    replace.push_back("--replace");
    CHECK(cli.run(replace).code == ExitCode::Ok);

    const RunResult check = cli.run({"content", "check", "--json"});
    INFO(check.out << check.err);
    CHECK(check.code == ExitCode::Ok);
    auto json = rv::core::parseJson(check.out, "check");
    REQUIRE(json.has_value());
    REQUIRE(json->at("consoles").size() == 1);
    CHECK(json->at("consoles")[0].at("system") == "snes");
    CHECK(json->at("consoles")[0].at("ok") == true);
}

TEST_CASE("cli/content: a model that fails the checks is refused and nothing is written", "[cli][content]") {
    const LoggingReset logging;
    const ContentCli cli;
    rv::test::FixtureConsoleOptions large;
    large.scale = 2.0F;
    std::vector<std::string> arguments = addArguments(cli.model("large.glb", large), "megadrive");
    arguments.resize(arguments.size() - 2); // no --scale-cm
    const RunResult refused = cli.run(arguments);
    CHECK(refused.code == ExitCode::ProblemsFound);
    CHECK(contains(refused.out, "megadrive: refused"));
    CHECK(contains(refused.out, "48.0 cm wide"));
    CHECK_FALSE(fs::exists(cli.consoles() / "megadrive"));

    // A folder put there by hand that fails: check says so, exit 1.
    REQUIRE(rv::test::writeFixtureConsole(cli.consoles() / "megadrive" / "console.glb", large).has_value());
    REQUIRE(rv::core::writeTextFileAtomic(cli.consoles() / "megadrive" / "console.json",
                                          rv::assets::writeConsoleManifest({}))
                .has_value());
    const RunResult check = cli.run({"content", "check", "--system", "megadrive"});
    CHECK(check.code == ExitCode::ProblemsFound);
    CHECK(contains(check.out, "megadrive: refused"));
}

TEST_CASE("cli/content: its arguments", "[cli][content]") {
    const LoggingReset logging;
    const ContentCli cli;
    CHECK(cli.run({"content"}).code == ExitCode::UsageError);
    CHECK(cli.run({"content", "remove"}).code == ExitCode::UsageError);
    CHECK(cli.run({"content", "add-console", "--system", "snes"}).code == ExitCode::UsageError);
    std::vector<std::string> gltf = addArguments(cli.model("x.glb"), "snes");
    gltf[5] = rv::core::pathToUtf8(fs::path(gltf[5]).replace_extension(".gltf"));
    CHECK(cli.run(gltf).code == ExitCode::UsageError);
    std::vector<std::string> badName = addArguments(cli.model("y.glb"), "bad/name");
    CHECK(cli.run(badName).code == ExitCode::UsageError);
    std::vector<std::string> badScale = addArguments(cli.model("z.glb"), "snes");
    badScale.back() = "24,7";
    CHECK(cli.run(badScale).code == ExitCode::UsageError);
    CHECK(cli.run({"content", "check", "--system", "nes"}).code == ExitCode::UsageError);
    CHECK(cli.run({"content", "--help"}).code == ExitCode::Ok);
}
