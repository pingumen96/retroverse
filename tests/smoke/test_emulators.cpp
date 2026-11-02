// The emulators screen (I7 step 4, ADR-0018 §5, §6, §12, §15), driven by the pad's actions alone through input scripts on
// the standard ES-DE fixture and read from the run report's "emulators" section, the catalog database and retroverse-cli:
// a system that requires configuration repaired and launched into the fake emulator (SC-10), its session a test; a crash
// leaving the choice pending; an unattended run without the allow-list refused before anything starts; the guard refusing
// the fixture's own copy of the fake; a program chosen in the browser, one refused; the wizard's page 5; no text cut at
// 125% at 1280 x 720; the ray in a headset. The guard lists the build's fake alone (ctest ENVIRONMENT); the shipped
// profiles a machine may find are never chosen.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr int kFirstPress = 10;
constexpr int kGap = 3;
constexpr std::string_view kBlockedCommand =
    R"(<command label="Fake Emulator">%EMULATOR_FAKE-EMULATOR% -- -L %CORE_FAKE-EMULATOR%\fake_gb_libretro.dll %ROM%</command>)";

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

struct Script {
    int frame = kFirstPress;
    Json steps = Json::array();

    Script& press(std::string_view action, int times = 1) {
        for (int index = 0; index < times; ++index) {
            steps.push_back(Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{std::string(action), 1}}}});
            frame += kGap;
        }
        return *this;
    }
    Script& wait(int frames) {
        frame += frames;
        return *this;
    }
    /// From the world into the emulators screen.
    Script& openScreen() { return press("menu").press("ui_down", 2).press("ui_activate").wait(3); }

    void write(const fs::path& file) const {
        REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    }
};

/// While it lives RV_LAUNCH_ALLOWLIST is not set, for the runs this process starts.
class NoAllowList {
public:
    NoAllowList() {
        const DWORD size = GetEnvironmentVariableW(L"RV_LAUNCH_ALLOWLIST", nullptr, 0);
        if (size > 0) {
            std::wstring value(size, L'\0');
            value.resize(GetEnvironmentVariableW(L"RV_LAUNCH_ALLOWLIST", value.data(), size));
            m_previous = std::move(value);
        }
        SetEnvironmentVariableW(L"RV_LAUNCH_ALLOWLIST", nullptr);
    }
    ~NoAllowList() { SetEnvironmentVariableW(L"RV_LAUNCH_ALLOWLIST", m_previous ? m_previous->c_str() : nullptr); }
    NoAllowList(const NoAllowList&) = delete;
    NoAllowList& operator=(const NoAllowList&) = delete;

private:
    std::optional<std::wstring> m_previous;
};

/// The standard fixture imported into <case>/data, gb's first command made one Retroverse does not run (with
/// `blocked`), and two profiles serving gb that run the build's fake.
struct Library {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit Library(std::string_view name, bool blocked = true)
        : fixture(rv::test::Fixture::materialize("esde/standard").value()),
          dir(freshCaseDir(std::string("emulators/") + std::string(name))), data(dir / "data") {
        if (blocked) {
            const fs::path systems = fixture.path("Programs/ES-DE/resources/systems/windows/es_systems.xml");
            std::string text = rv::core::readTextFile(systems).value();
            const std::size_t at = text.find(kBlockedCommand);
            REQUIRE(at != std::string::npos);
            text.replace(at, kBlockedCommand.size(),
                         R"(<command label="Script">%ESCAPESPECIALS% %EMULATOR_FAKE-EMULATOR% -- %ROM%</command>)");
            REQUIRE(rv::core::writeTextFileAtomic(systems, text).has_value());
        }
        cli("library", {"library", "create", "--home", arg(fixture.root()), "--install",
                        arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
        const auto profile = [](std::string_view id, std::vector<std::string> arguments) {
            arguments.insert(arguments.end(), {"--", "{rom}"});
            return Json{{"id", id},
                        {"executable", arg(fakeEmulatorExe())},
                        {"arguments", arguments},
                        {"windowTimeoutSeconds", 0},
                        {"hideConsoleWindow", true},
                        {"systems", {{"gb", Json::object()}}}};
        };
        const Json document{
            {"schemaVersion", 1},
            {"profiles", Json::array({profile("fake-return", {"--sleep-ms", "2600", "--exit-timeout-ms", "30000"}),
                                      profile("fake-crash", {"--crash"})})}};
        REQUIRE(rv::core::writeTextFileAtomic(data / "config" / "emulators.json", document.dump(2) + "\n").has_value());
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    /// `retroverse-cli assign` as JSON, its arguments `arguments`.
    [[nodiscard]] Json assign(std::string_view name, std::vector<std::string> arguments) const {
        const fs::path out = dir / (std::string(name) + ".json");
        arguments.insert(arguments.begin(), "assign");
        arguments.insert(arguments.end(), {"--json", "--output", arg(out)});
        cli(name, std::move(arguments));
        return Json::parse(rv::core::readTextFile(out).value());
    }

    /// Every play session, newest first.
    [[nodiscard]] std::vector<rv::catalog_db::ListedSession> sessions() const {
        rv::catalog_db::DatabaseOptions options;
        options.create = false;
        options.switchToWal = false;
        auto database = rv::catalog_db::Database::open(data / "database" / "retroverse.sqlite", options);
        REQUIRE(database.has_value());
        auto reader = database->openReader();
        REQUIRE(reader.has_value());
        auto store = rv::catalog_db::SessionStore::open(*reader);
        REQUIRE(store.has_value());
        return (*store)->sessions(rv::catalog_db::SessionFilter{}).value();
    }

    AppRun run(std::string_view name, const Script& script) const {
        const fs::path file = dir / (std::string(name) + ".script.json");
        script.write(file);
        const fs::path shot = dir / (std::string(name) + ".png");
        return runApp(retroverseExe(), dir, name,
                      {"--scene", "m0_testroom", "--exit-after-frames", "--frames", std::to_string(script.frame + 4),
                       "--fixed-dt", "0.016667", "--screenshot", arg(shot), "--input-script", arg(file)});
    }
};

const rv::app::EmulatorsSystemRecord& systemOf(const rv::app::EmulatorsRecord& record, std::string_view name) {
    const auto found = std::find_if(record.systems.begin(), record.systems.end(),
                                    [&](const auto& system) { return system.name == name; });
    REQUIRE(found != record.systems.end());
    return *found;
}

/// Where gb stands and its option `name`, read from a first run of the screen: the pad's presses down to each.
struct Rows {
    int system = 0;
    int option = 0;
};
Rows rowsOf(const Library& library, std::string_view option) {
    const AppRun probe = library.run("probe", Script{}.openScreen());
    skipWithoutOpenGl(probe);
    requireCleanRun(probe);
    INFO(probe.describe());
    REQUIRE(probe.report->emulators);
    const auto& systems = probe.report->emulators->systems;
    const auto gb = std::find_if(systems.begin(), systems.end(), [](const auto& row) { return row.name == "gb"; });
    REQUIRE(gb != systems.end());
    const auto found = std::find_if(gb->options.begin(), gb->options.end(), [&](const std::string& row) {
        return row.starts_with(std::string(option) + "=");
    });
    REQUIRE(found != gb->options.end());
    return Rows{static_cast<int>(gb - systems.begin()), static_cast<int>(found - gb->options.begin())};
}

/// From the world to the option at `rows`, its Choice view showing.
Script toChoice(const Rows& rows) {
    Script script;
    script.openScreen().press("ui_down", rows.system).press("ui_activate").wait(3);
    script.press("ui_down", rows.option).press("ui_activate").wait(3);
    return script;
}

std::vector<std::string> eventsOf(const rv::app::EmulatorsRecord& record) {
    std::vector<std::string> events;
    for (const rv::app::EmulatorsEventRecord& event : record.events) {
        events.push_back(event.detail.empty() ? event.event : event.event + ":" + event.detail);
    }
    return events;
}

bool holds(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

TEST_CASE("emulators: a system requiring configuration repaired from the pad and launched (SC-10)", "[gpu][emulators]") {
    ensureLaunchAllowList();
    const Library library("repaired");
    const Rows rows = rowsOf(library, "fake-return");

    const AppRun before = library.run("before", Script{}.openScreen());
    skipWithoutOpenGl(before);
    {
        const rv::app::EmulatorsSystemRecord& gb = systemOf(*before.report->emulators, "gb");
        CHECK(gb.readiness == "requires-configuration");
        CHECK(gb.blocker == "token-unsupported");
        CHECK(gb.source == "first-command");
        CHECK(gb.choice == "Script");
        REQUIRE_FALSE(gb.options.empty());
        // SC-10's suggestion: the ready ones first.
        const auto ready = std::partition_point(gb.options.begin(), gb.options.end(), [](const std::string& option) {
            return option.ends_with("=ready");
        });
        CHECK(ready != gb.options.begin());
        CHECK(std::none_of(ready, gb.options.end(),
                           [](const std::string& option) { return option.ends_with("=ready"); }));
        CHECK(holds(gb.options, "Script=requires-configuration"));
        CHECK(holds(gb.options, "fake-return=ready"));
    }

    Script script = toChoice(rows);
    script.press("ui_activate").wait(6); // Prova: the fake runs, returns; the frames wait for it
    const AppRun run = library.run("run", script);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->emulators);
    const rv::app::EmulatorsRecord& emulators = *run.report->emulators;
    CHECK(emulators.tests == 1);
    CHECK(emulators.testsRefused == 0);
    CHECK(emulators.testsStored == 1);
    CHECK(emulators.verdict == "verified");
    CHECK(emulators.view == "system");
    CHECK(emulators.textCut == 0);
    const rv::app::EmulatorsSystemRecord& gb = systemOf(emulators, "gb");
    CHECK(gb.readiness == "ready");
    CHECK(gb.source == "retroverse-system");
    CHECK(gb.verified);
    CHECK(gb.stored);

    REQUIRE(run.report->launches.size() == 1);
    CHECK(run.report->launches.front().state == "Returned");
    CHECK(run.report->launches.front().verified);
    const auto sessions = library.sessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions.front().session.test);
    CHECK(sessions.front().session.outcome == rv::catalog::PlaySessionOutcome::Returned);
    CHECK(sessions.front().session.choice.value == "fake-return");
    CHECK(library.assign("stored", {"gb"}).at("choice").at("value") == "fake-return");

    // It stands across a restart, verified, with nothing launched; and "ES-DE" takes it back.
    const AppRun again = library.run("again", Script{}.openScreen());
    requireCleanRun(again);
    CHECK(systemOf(*again.report->emulators, "gb").verified);
    Script clear;
    clear.openScreen().press("ui_down", rows.system).press("ui_activate").wait(3).press("ui_end").press("ui_activate").wait(6);
    const AppRun cleared = library.run("cleared", clear);
    requireCleanRun(cleared);
    const rv::app::EmulatorsSystemRecord& back = systemOf(*cleared.report->emulators, "gb");
    CHECK_FALSE(back.stored);
    CHECK(back.readiness == "requires-configuration");
    CHECK(library.assign("cleared", {"gb"}).at("choice").is_null());
}

TEST_CASE("emulators: a crash leaves the choice pending, recorded as a test", "[gpu][emulators]") {
    ensureLaunchAllowList();
    const Library library("crash");
    const Rows rows = rowsOf(library, "fake-crash");
    Script script = toChoice(rows);
    script.press("ui_activate").wait(6);
    const AppRun run = library.run("run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->emulators);
    const rv::app::EmulatorsRecord& emulators = *run.report->emulators;
    CHECK(emulators.tests == 1);
    CHECK(emulators.testsStored == 0);
    CHECK(emulators.verdict == "crashed");
    CHECK(emulators.view == "choice");
    CHECK(systemOf(emulators, "gb").readiness == "requires-configuration");
    const auto sessions = library.sessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions.front().session.test);
    CHECK(sessions.front().session.outcome != rv::catalog::PlaySessionOutcome::Returned);
    CHECK(library.assign("stored", {"gb"}).at("choice").is_null());
}

TEST_CASE("emulators: an unattended run without the allow-list refuses a launch test before anything starts",
          "[gpu][emulators]") {
    const Library library("unattended");
    const Rows rows = rowsOf(library, "fake-return");
    Script script = toChoice(rows);
    script.press("ui_activate").wait(6);
    const NoAllowList unset;
    const AppRun run = library.run("run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->emulators);
    const rv::app::EmulatorsRecord& emulators = *run.report->emulators;
    CHECK(emulators.tests == 0);
    CHECK(emulators.testsRefused == 1);
    CHECK(emulators.verdict == "unattended");
    CHECK(emulators.view == "choice");
    CHECK(holds(eventsOf(emulators), "test-refused"));
    CHECK(run.report->launches.empty()); // nothing was even resolved
    CHECK(library.sessions().empty());
    CHECK(library.assign("stored", {"gb"}).at("choice").is_null());
}

TEST_CASE("emulators: the guard applies as to every launch: the fixture's copy of the fake is refused", "[gpu][emulators]") {
    ensureLaunchAllowList();
    const Library library("guard", false);
    const Rows rows = rowsOf(library, "Fake Emulator");
    Script script = toChoice(rows);
    script.press("ui_activate").wait(6);
    const AppRun run = library.run("run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->emulators);
    const rv::app::EmulatorsRecord& emulators = *run.report->emulators;
    CHECK(emulators.tests == 1);
    CHECK(emulators.testsStored == 0);
    CHECK(emulators.verdict == "guard");
    const auto sessions = library.sessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions.front().session.refusedByGuard);
    CHECK(sessions.front().session.test);
    CHECK(library.assign("stored", {"gb"}).at("choice").is_null());
}

TEST_CASE("emulators: Salva senza provare stores it, shown as not verified", "[gpu][emulators]") {
    ensureLaunchAllowList();
    const Library library("saved");
    const Rows rows = rowsOf(library, "fake-return");
    Script script = toChoice(rows);
    script.press("ui_down").press("ui_activate").wait(6); // from Prova down to Salva senza provare
    const AppRun run = library.run("run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::EmulatorsRecord& emulators = *run.report->emulators;
    CHECK(emulators.tests == 0);
    CHECK(emulators.view == "system");
    const rv::app::EmulatorsSystemRecord& gb = systemOf(emulators, "gb");
    CHECK(gb.readiness == "ready");
    CHECK(gb.stored);
    CHECK_FALSE(gb.verified);
    CHECK(run.report->launches.empty());
    CHECK(library.sessions().empty());
    CHECK(library.assign("stored", {"gb"}).at("choice").at("value") == "fake-return");
}

TEST_CASE("emulators: a program chosen in the browser repairs a command that finds none; a shell is refused",
          "[gpu][emulators]") {
    ensureLaunchAllowList();
    const Library library("program", false);
    const fs::path shell = library.fixture.path("Programs/ES-DE/Emulators/FakeEmulator/powershell.exe");
    fs::copy_file(fakeEmulatorExe(), shell);
    const Rows rows = rowsOf(library, "Missing Emulator (Standalone)");
    const Script choice = toChoice(rows);

    // The browser starts at ES-DE's install folder, its folders first: Emulators, then FakeEmulator, then the programs.
    Script refused = choice;
    refused.press("ui_activate").wait(3).press("ui_activate").wait(3).press("ui_activate").wait(3);
    refused.press("ui_down").press("ui_activate").wait(6); // powershell.exe
    const AppRun run = library.run("refused", refused);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->emulators);
    CHECK(run.report->emulators->problem == "refused");
    CHECK(run.report->emulators->view == "choice");
    CHECK(run.report->emulators->browserTextCut == 0);
    CHECK(library.assign("nothing", {"--emulator", "MISSING-EMULATOR"}).at("program").is_null());

    Script chosen = choice;
    chosen.press("ui_activate").wait(3).press("ui_activate").wait(3).press("ui_activate").wait(3);
    chosen.press("ui_activate").wait(6); // fake-emulator.exe
    const AppRun made = library.run("chosen", chosen);
    requireCleanRun(made);
    INFO(made.describe());
    CHECK(made.report->emulators->problem.empty());
    CHECK(made.report->emulators->view == "choice");
    CHECK(library.assign("stored", {"--emulator", "MISSING-EMULATOR"}).at("program") ==
          arg(library.fixture.path("Programs/ES-DE/Emulators/FakeEmulator/fake-emulator.exe")));
    const auto& options = systemOf(*made.report->emulators, "gb").options;
    CHECK(holds(options, "Missing Emulator (Standalone)=ready"));
}

TEST_CASE("emulators: without a catalog the screen says so and walks back", "[gpu][emulators]") {
    const fs::path dir = freshCaseDir("emulators/no_catalog");
    Script script;
    script.openScreen().wait(3);
    const fs::path file = dir / "script.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), dir, "run",
                              {"--scene", "m0_testroom", "--exit-after-frames", "--frames",
                               std::to_string(script.frame + 2), "--fixed-dt", "0.016667", "--input-script", arg(file)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->emulators);
    CHECK(run.report->emulators->opened == 1);
    CHECK(run.report->emulators->state == "unavailable");
    CHECK(run.report->emulators->open);
}

TEST_CASE("emulators: the wizard's page 5 opens the screen and takes the run on", "[gpu][emulators][wizard]") {
    const fs::path dir = freshCaseDir("emulators/wizard");
    const rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    Script script;
    script.frame = 5;
    script.press("ui_activate", 4).wait(3); // Benvenuto, Rilevamento, Anteprima, Importazione
    script.press("ui_up").press("ui_activate").wait(4); // Apri Emulatori
    script.press("ui_activate").wait(3).press("ui_back").press("ui_back").wait(3); // a system, and back out
    script.press("ui_down").press("ui_activate", 2).press("ui_activate").wait(20); // Avanti, Avanti, Entra
    const fs::path file = dir / "script.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), dir, "run",
                              {"--wizard", "--esde-home", arg(fixture.root()), "--esde-install",
                               arg(fixture.path("Programs/ES-DE")), "--exit-after-frames", "--frames",
                               std::to_string(script.frame + 2), "--fixed-dt", "0.016667", "--input-script", arg(file)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->emulators);
    CHECK(run.report->emulators->opened == 1);
    CHECK(run.report->emulators->state == "listed");
    CHECK_FALSE(run.report->emulators->open);
    CHECK(run.report->emulators->textCut == 0);
    REQUIRE(run.report->emulators->systems.size() == 1);
    CHECK(run.report->emulators->systems.front().name == "snes");
    REQUIRE(run.report->wizard);
    CHECK(run.report->wizard->entered);
    CHECK(run.report->wizard->pages == std::vector<std::string>{"welcome", "detection", "preview", "import", "emulators",
                                                                "appearance", "world"});
}

TEST_CASE("emulators: at text scale 125% at 1280 x 720 no view and no browser is cut", "[gpu][emulators]") {
    ensureLaunchAllowList();
    const Library library("scale125");
    REQUIRE(rv::core::writeTextFileAtomic(library.data / "settings.json",
                                          R"({"schemaVersion": 1, "player": {"textScale": 1.25}})")
                .has_value());
    const Rows rows = rowsOf(library, "Missing Emulator (Standalone)");
    Script script = toChoice(rows);
    script.press("ui_activate").wait(4).press("ui_activate").wait(4).press("ui_back").wait(3);
    const fs::path file = library.dir / "scale.script.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), library.dir, "scale",
                              {"--scene", "m0_testroom", "--width", "1280", "--height", "720", "--exit-after-frames",
                               "--frames", std::to_string(script.frame + 4), "--fixed-dt", "0.016667", "--input-script",
                               arg(file)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->emulators);
    CHECK(run.report->emulators->textCut == 0);
    CHECK(run.report->emulators->browserTextCut == 0);
}

TEST_CASE("emulators: in a headset the panel shows and the ray opens a system", "[gpu][emulators][xr]") {
    const Library library("vr");
    // The right controller at the hip points at the first row, in the panel's upper left (the canvas stands 1.3 m in
    // front of the eyes, 0.12 m below them, 1.8 m wide at the window's 16:9).
    const glm::vec3 hand(0.2f, 1.25f, -0.3f);
    const glm::vec3 target(-0.2f, 1.65f - 0.12f + 0.27f, -1.3f);
    const glm::vec3 toTarget = glm::normalize(target - hand);
    const float yawDeg = rv::core::radiansToDegrees(std::atan2(toTarget.x, -toTarget.z));
    const float pitchDeg = rv::core::radiansToDegrees(std::asin(toTarget.y));
    const std::string pose = fmt::format(R"("rightHand": {{"yaw": {}, "pitch": {}, "position": [{}, {}, {}]}})", yawDeg,
                                         pitchDeg, hand.x, hand.y, hand.z);
    const fs::path script = library.dir / "vr.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, fmt::format(R"([
  {{"fromFrame": 0, "toFrame": 79, "headPose": {{"position": [0, 1.65, 0]}}}},
  {{"fromFrame": 0, "toFrame": 79, {0}}},
  {{"fromFrame": 10, "toFrame": 10, "actions": {{"menu": 1}}}},
  {{"fromFrame": 13, "toFrame": 13, "actions": {{"ui_down": 1}}}},
  {{"fromFrame": 16, "toFrame": 16, "actions": {{"ui_down": 1}}}},
  {{"fromFrame": 19, "toFrame": 19, "actions": {{"ui_activate": 1}}}},
  {{"fromFrame": 50, "toFrame": 53, "rightSelect": 1}}
])",
                                                              pose))
                .has_value());
    const AppRun run = runApp(retroverseExe(), library.dir, "run",
                              {"--scene", "m0_testroom", "--vr", "--xr-runtime", "fake", "--exit-after-frames",
                               "--frames", "80", "--fixed-dt", "0.016667", "--screenshot",
                               arg(library.dir / "eyes.png"), "--input-script", arg(script)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->xr);
    CHECK(run.report->xr->uiClicks == 1);
    REQUIRE(run.report->emulators);
    CHECK(run.report->emulators->view == "system");
    CHECK(run.report->launches.empty());
}

// By hand, for a look: rv_smoke_tests "[emulators_shots]" with RV_SHOTS_DIR naming a folder writes each view, hidden and
// frame-limited, the pad's actions alone. Not a check: visual verification.
TEST_CASE("emulators: the pictures", "[.shots][.emulators_shots]") {
    const char* folder = std::getenv("RV_SHOTS_DIR");
    if (folder == nullptr) {
        SKIP("RV_SHOTS_DIR names no folder");
    }
    ensureLaunchAllowList();
    const fs::path out = rv::core::pathFromUtf8(folder);
    fs::create_directories(out);
    const Library library("shots");
    const Rows ready = rowsOf(library, "fake-return");
    const Rows missing = rowsOf(library, "Missing Emulator (Standalone)");
    REQUIRE(rv::core::writeTextFileAtomic(library.data / "settings.json", "{\"schemaVersion\": 1}").has_value());
    const auto all = [&](std::string_view prefix, const std::vector<std::string>& size) {
        int number = 0;
        const auto shoot = [&](std::string_view name, const Script& script) {
            const std::string run = fmt::format("{}{:02}_{}", prefix, ++number, name);
            const fs::path file = library.dir / (run + ".script.json");
            script.write(file);
            std::vector<std::string> arguments{"--scene", "m0_testroom", "--exit-after-frames", "--frames",
                                               std::to_string(script.frame + 4), "--fixed-dt", "0.016667", "--screenshot",
                                               arg(out / (run + ".png")), "--input-script", arg(file)};
            arguments.insert(arguments.end(), size.begin(), size.end());
            const AppRun result = runApp(retroverseExe(), library.dir, run, std::move(arguments));
            requireCleanRun(result);
            INFO(result.describe());
            REQUIRE(result.report->emulators);
            CHECK(result.report->emulators->textCut == 0);
            CHECK(result.report->emulators->browserTextCut == 0);
        };
        shoot("systems", Script{}.openScreen());
        shoot("system", Script{}.openScreen().press("ui_down", ready.system).press("ui_activate").wait(3));
        shoot("choice_ready", toChoice(ready));
        shoot("choice_missing", toChoice(missing));
        shoot("browser", Script(toChoice(missing)).press("ui_activate").wait(4));
        shoot("browser_programs", Script(toChoice(missing)).press("ui_activate").wait(4).press("ui_activate").wait(4).press("ui_activate").wait(4));
    };
    all("", {"--width", "1920", "--height", "1080"});
    REQUIRE(rv::core::writeTextFileAtomic(library.data / "settings.json",
                                          R"({"schemaVersion": 1, "player": {"textScale": 1.25}})")
                .has_value());
    all("scale125_", {"--width", "1920", "--height", "1080"});
    all("scale125_720_", {"--width", "1280", "--height", "720", "--screenshot-size", "1280x720"});
}
