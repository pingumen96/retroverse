// retroverse-cli identity show (I11 step 1, ADR-0019 §2): the standard fixture imported, identity.json pointing at the
// esde/theme fixture's theme folders, each system's identity with its sources as text, as one system's details and as
// JSON; and the exit codes (no library.json, an unknown system, an identity.json that cannot be read, a subcommand
// that does not exist).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/catalog/library.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/discovery.hpp>

#include "support/fixture.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

rv::core::PathEnvironment testEnvironment(const fs::path& executableDir) {
    rv::core::PathEnvironment environment;
    environment.executableDir = executableDir;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return environment;
}

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

std::string jsonPath(const fs::path& path) {
    std::string escaped;
    for (const char c : rv::core::pathToUtf8(path)) {
        escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
    }
    return escaped;
}

/// The standard fixture imported into a data directory, and the theme fixture beside it.
class IdentityCli {
public:
    IdentityCli() : m_dir("rv-cli-identity"), m_data(m_dir.file("data")) {
        auto standard = rv::test::Fixture::materialize("esde/standard");
        REQUIRE(standard.has_value());
        m_standard.emplace(std::move(*standard));
        auto themes = rv::test::Fixture::materialize("esde/theme");
        REQUIRE(themes.has_value());
        m_themes.emplace(std::move(*themes));
    }

    void writeLibrary() const {
        rv::catalog::LibraryProfile profile;
        profile.id = rv::catalog::LibraryId::parse("esde").value();
        profile.esdeHome = m_standard->path("ES-DE");
        profile.installFolder = m_standard->path("Programs/ES-DE");
        REQUIRE(rv::esde::writeLibraryFile(m_data / "config" / "library.json", profile,
                                           rv::esde::ExistingFile::Replace)
                    .has_value());
    }
    void writeIdentity(std::string_view text) const {
        REQUIRE(rv::core::writeTextFileAtomic(m_data / "config" / "identity.json", text).has_value());
    }
    void writeThemeIdentity() const {
        writeIdentity(fmt::format(R"({{"schemaVersion": 1, "theme": "chosen-fixture-es-de",
  "themeRoots": ["{}", "{}"]}})",
                                  jsonPath(m_themes->path("ES-DE/themes")),
                                  jsonPath(m_themes->path("Programs/ES-DE/themes"))));
    }

    [[nodiscard]] RunResult run(std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(m_data)});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, testEnvironment(m_dir.path()));
        return RunResult{code, out.text(), err.text()};
    }

private:
    rv::test::TempDir m_dir;
    fs::path m_data;
    std::optional<rv::test::Fixture> m_standard;
    std::optional<rv::test::Fixture> m_themes;
};

} // namespace

TEST_CASE("cli/identity: show lists each system's identity with its sources", "[cli][identity]") {
    const LoggingReset loggingReset;
    const IdentityCli cli;

    SECTION("without a library.json, a usage error") {
        const RunResult result = cli.run({"identity", "show"});
        CHECK(result.code == ExitCode::UsageError);
        CHECK(contains(result.err, "library create"));
    }

    cli.writeLibrary();
    const RunResult imported = cli.run({"import"});
    INFO(imported.err);
    REQUIRE(imported.code == ExitCode::Ok);

    SECTION("no identity.json: ES-DE's full names, no theme reaches the fixture") {
        const RunResult result = cli.run({"identity", "show"});
        INFO(result.err);
        REQUIRE(result.code == ExitCode::Ok);
        CHECK(contains(result.out, "Nintendo Game Boy"));
        CHECK(contains(result.out, "ES-DE's full name"));
        CHECK(contains(result.out, ": 0 named by a theme or identity.json, 0 with a logo, 0 with a description"));
    }
    SECTION("identity.json's theme roots: the chosen theme's name, its sources in the text, the JSON and the details") {
        cli.writeThemeIdentity();
        const RunResult text = cli.run({"identity", "show"});
        INFO(text.err);
        REQUIRE(text.code == ExitCode::Ok);
        CHECK(contains(text.out, "Pocket Lumen"));
        CHECK(contains(text.out, "theme chosen-fixture-es-de"));
        CHECK(contains(text.out, "Lumen 16"));
        CHECK(contains(text.out, "theme linear-es-de"));
        CHECK(contains(text.out, "[1 left out]")); // gb's linear-es-de logo is too large
        // gb from the chosen theme, snes from linear-es-de (its logo and Italian description), psx its full name.
        CHECK(contains(text.out, ": 2 named by a theme or identity.json, 1 with a logo, 1 with a description"));

        const RunResult details = cli.run({"identity", "show", "--system", "gb"});
        INFO(details.err);
        REQUIRE(details.code == ExitCode::Ok);
        CHECK(contains(details.out, "gb (theme id gb)"));
        CHECK(contains(details.out, "Handheld  [theme chosen-fixture-es-de]"));
        CHECK(contains(details.out, "#5E8E7A"));
        CHECK(contains(details.out, "warning: logo-too-large"));

        const RunResult json = cli.run({"identity", "show", "--json"});
        REQUIRE(json.code == ExitCode::Ok);
        const auto parsed = rv::core::parseJson(json.out, "stdout");
        REQUIRE(parsed.has_value());
        REQUIRE((*parsed)["metadataThemes"].size() == 2);
        std::vector<std::string> seen;
        for (const auto& system : (*parsed)["systems"]) {
            seen.push_back(system["system"].get<std::string>());
            if (system["system"] == "gb") {
                CHECK(system["name"]["value"] == "Pocket Lumen");
                CHECK(system["name"]["source"] == "theme");
                CHECK(system["name"]["origin"] == "chosen-fixture-es-de");
                CHECK(system["logo"]["source"] == "none");
                CHECK(system["issues"].size() == 1);
            } else if (system["system"] == "snes") {
                CHECK(system["name"]["value"] == "Lumen 16");
                CHECK(system["name"]["origin"] == "linear-es-de");
                CHECK(system["logo"]["format"] == "svg");
                CHECK(system["descriptionLanguage"] == "it_IT");
            } else {
                CHECK(system["name"]["source"] == "full-name");
            }
        }
        std::sort(seen.begin(), seen.end());
        CHECK(seen == std::vector<std::string>{"gb", "psx", "snes"});
    }
    SECTION("an unknown system, an unknown subcommand, an identity.json that cannot be read") {
        CHECK(cli.run({"identity", "show", "--system", "nosuch"}).code == ExitCode::UsageError);
        CHECK(cli.run({"identity", "list"}).code == ExitCode::UsageError);
        CHECK(cli.run({"identity"}).code == ExitCode::UsageError);
        CHECK(cli.run({"identity", "--help"}).code == ExitCode::Ok);
        cli.writeIdentity(R"({"schemaVersion": 1, "colour": "red"})");
        const RunResult broken = cli.run({"identity", "show"});
        CHECK(broken.code == ExitCode::RuntimeFailure);
        CHECK(contains(broken.err, "unknown key \"colour\""));
    }
}
