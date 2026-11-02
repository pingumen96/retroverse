// The catalog worker's jobs for the emulators screen (I7 step 4, ADR-0018 §5) on the standard ES-DE fixture: the table, a
// launch test's pending choice stored only after a clean return, a stale label, a verification that does not count for
// another program, "Salva senza provare", a program chosen, refused by name and by the file it opens. Nothing starts: the
// play sessions are written as the app writes them around a launch.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rv/app/catalog_worker.hpp>
#include <rv/app/emulators_data.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/core/file_io.hpp>

#include "catalog_test_support.hpp"
#include "support/program_stub.hpp"

using rv::app::CatalogResults;
using rv::app::CatalogState;
using rv::app::CatalogWorker;
using rv::app::ChoiceChange;
using rv::app::LaunchTestChoice;
using rv::catalog::PlaySession;
using rv::catalog::PlaySessionOutcome;
using rv::launcher::LaunchChoice;
using rv::launcher::LaunchChoiceKind;
using rv::test::ImportedFixture;

namespace {

const rv::catalog::LibraryId kLibrary = rv::catalog::LibraryId::parse("esde").value();
const rv::catalog::SystemName kGb = rv::catalog::SystemName::parse("gb").value();

LaunchChoice command(std::string label) {
    return LaunchChoice{LaunchChoiceKind::EsdeCommand, std::move(label)};
}

/// A worker on the fixture, its opening waited for, what it answers gathered.
struct Worker {
    explicit Worker(const ImportedFixture& fixture) : worker(optionsOf(fixture)) {
        wait([&] { return all.opened.has_value(); });
        REQUIRE(all.opened->state == CatalogState::Ready);
    }

    static CatalogWorker::Options optionsOf(const ImportedFixture& fixture) {
        CatalogWorker::Options options;
        options.paths = fixture.paths;
        options.owner = rv::catalog::SessionOwner{ImportedFixture::testOwnerProcessId(), 1};
        return options;
    }

    void gather() {
        CatalogResults results = worker.take();
        if (results.opened) {
            all.opened = std::move(results.opened);
        }
        if (results.emulatorTable) {
            all.emulatorTable = std::move(results.emulatorTable);
        }
        if (results.emulatorSamples) {
            all.emulatorSamples = std::move(results.emulatorSamples);
        }
        if (results.resolved) {
            all.resolved = std::move(results.resolved);
        }
        for (auto& launch : results.launches) {
            all.launches.push_back(std::move(launch));
        }
        for (auto& test : results.tests) {
            all.tests.push_back(std::move(test));
        }
        for (auto& change : results.changes) {
            all.changes.push_back(std::move(change));
        }
        for (auto& session : results.sessions) {
            all.sessions.push_back(std::move(session));
        }
    }

    template <typename Done>
    void wait(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + rv::test::kSettleTimeout;
        while (true) {
            gather();
            if (done()) {
                return;
            }
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    [[nodiscard]] rv::ui::EmulatorSystem table(std::string_view system) {
        all.emulatorTable.reset();
        worker.emulatorTable(1);
        wait([&] { return all.emulatorTable.has_value(); });
        REQUIRE_FALSE(all.emulatorTable->error.has_value());
        const auto& rows = all.emulatorTable->systems;
        const auto row = std::find_if(rows.begin(), rows.end(),
                                      [&](const rv::ui::EmulatorSystem& shown) { return shown.name == system; });
        REQUIRE(row != rows.end());
        return *row;
    }

    /// A launch test asked and its resolution awaited.
    rv::app::ResolvedLaunch resolveTest(rv::catalog::GameId game, const LaunchChoice& choice, std::uint64_t token) {
        all.launches.clear();
        worker.resolve(token, game, LaunchTestChoice{choice, std::nullopt});
        wait([&] { return !all.launches.empty(); });
        return all.launches.front();
    }

    /// The test's session as the app writes it, `finish` its end; how the test ended.
    rv::app::LaunchTestResult finishTest(const rv::app::ResolvedLaunch& resolved, void (*finish)(PlaySession&),
                                         const std::filesystem::path& program) {
        all.tests.clear();
        PlaySession session = resolved.session;
        session.started = std::chrono::system_clock::now();
        session.outcome = PlaySessionOutcome::Running;
        worker.beginSession(resolved.token, session);
        finish(session);
        session.ended = session.started + std::chrono::seconds(30);
        worker.finishSession(resolved.token, session, resolved.system, program);
        wait([&] { return !all.tests.empty(); });
        return all.tests.front();
    }

    void change(ChoiceChange asked, std::uint64_t serial = 1) {
        all.changes.clear();
        worker.changeChoice(serial, std::move(asked));
        wait([&] { return !all.changes.empty(); });
    }

    CatalogWorker worker;
    CatalogResults all;
};

void cleanReturn(PlaySession& session) {
    session.outcome = PlaySessionOutcome::Returned;
    session.exitCode = 0;
    session.exitKind = rv::launcher::ExitKind::Success;
}

void crash(PlaySession& session) {
    session.outcome = PlaySessionOutcome::Crashed;
    session.exitCode = 0xC0000005u;
    session.exitKind = rv::launcher::ExitKind::Crash;
}

void quickExit(PlaySession& session) {
    cleanReturn(session);
    session.quickExit = true;
}

std::optional<LaunchChoice> storedChoice(const ImportedFixture& fixture, const rv::catalog::SystemName& system) {
    auto database = fixture.openDatabase();
    auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
    const auto choice = store->systemChoice(kLibrary, system).value();
    return choice ? std::optional<LaunchChoice>(choice->choice) : std::nullopt;
}

} // namespace

TEST_CASE("app/emulators: the table: each system's readiness and options, the ready first", "[app][emulators]") {
    const ImportedFixture fixture("rv-emulators");
    Worker worker(fixture);
    const rv::ui::EmulatorSystem gb = worker.table("gb");
    CHECK(gb.fullName == "Nintendo Game Boy");
    CHECK(gb.readiness == "ready");
    CHECK(gb.source == "first-command");
    CHECK(gb.choice == "Fake Emulator");
    CHECK_FALSE(gb.stored);
    CHECK_FALSE(gb.verified);
    REQUIRE(gb.options.size() >= 5);
    const auto ready = std::partition_point(gb.options.begin(), gb.options.end(), [](const rv::ui::EmulatorOption& option) {
        return option.readiness == "ready";
    });
    CHECK(ready != gb.options.begin());
    CHECK(ready != gb.options.end());
    CHECK(gb.options.front().name == "Fake Emulator");
    CHECK(gb.options.front().chosen);
    const auto missing = std::find_if(gb.options.begin(), gb.options.end(), [](const rv::ui::EmulatorOption& option) {
        return option.name == "Missing Emulator (Standalone)";
    });
    REQUIRE(missing != gb.options.end());
    CHECK(missing->readiness == "emulator-not-found");
    CHECK(missing->emulator == "MISSING-EMULATOR");
    const rv::ui::EmulatorSystem psx = worker.table("psx");
    CHECK(psx.stored); // the fixture's assign
    CHECK(psx.source == "retroverse-system");
}

TEST_CASE("app/emulators: a pending choice is stored only after a clean return", "[app][emulators]") {
    const ImportedFixture fixture("rv-emulators");
    const auto game = fixture.gameAt("gb", "Tiny Tennis (Europe).gb");
    const LaunchChoice home = command("Fake Emulator (Game Folder)");
    Worker worker(fixture);

    // A crash: not stored, the session a test's all the same.
    const rv::app::ResolvedLaunch first = worker.resolveTest(game, home, 1);
    CHECK_FALSE(first.error.has_value());
    CHECK(first.session.test);
    CHECK(first.session.choice == home);
    CHECK(first.session.choiceSource == rv::catalog::ChoiceSource::RetroverseSystem);
    const auto program = first.resolution.translation.program->path;
    const rv::app::LaunchTestResult crashed = worker.finishTest(first, &crash, program);
    CHECK(crashed.verdict == "crashed");
    CHECK_FALSE(crashed.stored);
    CHECK_FALSE(storedChoice(fixture, kGb).has_value());

    // A quick exit is no return either.
    const rv::app::ResolvedLaunch second = worker.resolveTest(game, home, 2);
    const rv::app::LaunchTestResult quick = worker.finishTest(second, &quickExit, program);
    CHECK(quick.verdict == "quick-exit");
    CHECK_FALSE(storedChoice(fixture, kGb).has_value());

    // A clean return stores it, verified, and the systems are resolved again.
    const rv::app::ResolvedLaunch third = worker.resolveTest(game, home, 3);
    worker.all.resolved.reset();
    const rv::app::LaunchTestResult clean = worker.finishTest(third, &cleanReturn, program);
    CHECK(clean.verdict == "verified");
    CHECK(clean.stored);
    REQUIRE(storedChoice(fixture, kGb).has_value());
    CHECK(*storedChoice(fixture, kGb) == home);
    worker.wait([&] { return worker.all.resolved.has_value(); });
    CHECK(worker.all.resolved->generation > 0);
    const rv::ui::EmulatorSystem gb = worker.table("gb");
    CHECK(gb.stored);
    CHECK(gb.source == "retroverse-system");
    CHECK(gb.verified);

    auto database = fixture.openDatabase();
    auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
    const auto sessions = store->sessions(rv::catalog_db::SessionFilter{}).value();
    REQUIRE(sessions.size() == 3);
    CHECK(std::all_of(sessions.begin(), sessions.end(),
                      [](const rv::catalog_db::ListedSession& listed) { return listed.session.test; }));
}

TEST_CASE("app/emulators: a test launches with its choice over the game's own, and a stale label blocks it",
          "[app][emulators]") {
    const ImportedFixture fixture("rv-emulators");
    const auto game = fixture.gameAt("gb", "Tiny Tennis (Europe).gb");
    {
        auto database = fixture.openDatabase();
        auto store = rv::catalog_db::SessionStore::open(database.writer()).value();
        REQUIRE(store->setGameChoice(game, command("Home Emulator"), std::chrono::system_clock::now()).has_value());
    }
    Worker worker(fixture);
    const rv::app::ResolvedLaunch tested = worker.resolveTest(game, command("Fake Emulator"), 1);
    CHECK(tested.resolution.chosen.choice == command("Fake Emulator"));

    const rv::app::ResolvedLaunch stale = worker.resolveTest(game, command("No Such Command"), 2);
    CHECK(stale.resolution.readiness() == rv::catalog::LaunchReadiness::RequiresConfiguration);
    REQUIRE_FALSE(stale.resolution.translation.blockers.empty());
    CHECK(stale.resolution.translation.blockers.front().code == "choice-stale");
    CHECK_FALSE(stale.resolution.request().has_value());
}

TEST_CASE("app/emulators: a verification does not count for another program", "[app][emulators]") {
    const ImportedFixture fixture("rv-emulators");
    const auto game = fixture.gameAt("gb", "Tiny Tennis (Europe).gb");
    Worker worker(fixture);
    const LaunchChoice choice = command("Fake Emulator");
    const rv::app::ResolvedLaunch tested = worker.resolveTest(game, choice, 1);
    const auto program = tested.resolution.translation.program->path;
    REQUIRE(worker.finishTest(tested, &cleanReturn, program).verdict == "verified");
    CHECK(worker.table("gb").verified);

    // Another program for the same emulator: the same choice, not verified any more.
    const rv::test::TempDir other("rv-emulators-program");
    const std::filesystem::path stub = other.file("another-emulator.exe");
    rv::test::writeProgramStub(stub);
    ChoiceChange change;
    change.kind = ChoiceChange::Kind::SetProgram;
    change.emulator = "FAKE-EMULATOR";
    change.program = stub;
    worker.change(std::move(change));
    CHECK(worker.all.changes.front().problem.empty());
    const rv::ui::EmulatorSystem gb = worker.table("gb");
    CHECK(gb.readiness == "ready");
    CHECK_FALSE(gb.verified);
    REQUIRE_FALSE(gb.options.empty());
    CHECK(gb.options.front().programChosen);
    CHECK_FALSE(gb.options.front().verified);

    // Its program removed: the verification counts again.
    ChoiceChange cleared;
    cleared.kind = ChoiceChange::Kind::ClearProgram;
    cleared.emulator = "FAKE-EMULATOR";
    worker.change(std::move(cleared));
    CHECK(worker.table("gb").verified);
}

TEST_CASE("app/emulators: Salva senza provare stores it unverified; Clear gives ES-DE's back", "[app][emulators]") {
    const ImportedFixture fixture("rv-emulators");
    Worker worker(fixture);
    ChoiceChange save;
    save.kind = ChoiceChange::Kind::Save;
    save.system = kGb;
    save.choice = command("Home Emulator");
    worker.change(std::move(save));
    CHECK(worker.all.changes.front().problem.empty());
    REQUIRE(storedChoice(fixture, kGb).has_value());
    CHECK(storedChoice(fixture, kGb)->value == "Home Emulator");
    rv::ui::EmulatorSystem gb = worker.table("gb");
    CHECK(gb.stored);
    CHECK_FALSE(gb.verified);

    ChoiceChange clear;
    clear.kind = ChoiceChange::Kind::Clear;
    clear.system = kGb;
    worker.change(std::move(clear));
    CHECK_FALSE(storedChoice(fixture, kGb).has_value());
    gb = worker.table("gb");
    CHECK_FALSE(gb.stored);
    CHECK(gb.source == "first-command");
}

TEST_CASE("app/emulators: a program for a command that finds none, refused by name and by the file it opens",
          "[app][emulators]") {
    const ImportedFixture fixture("rv-emulators");
    Worker worker(fixture);
    const auto missingOf = [](const rv::ui::EmulatorSystem& gb) {
        return *std::find_if(gb.options.begin(), gb.options.end(), [](const rv::ui::EmulatorOption& option) {
            return option.emulator == "MISSING-EMULATOR";
        });
    };
    CHECK(missingOf(worker.table("gb")).readiness == "emulator-not-found");

    const rv::test::TempDir other("rv-emulators-program");
    const auto choose = [&](const std::filesystem::path& program) {
        ChoiceChange change;
        change.kind = ChoiceChange::Kind::SetProgram;
        change.emulator = "MISSING-EMULATOR";
        change.program = program;
        worker.change(std::move(change));
        return worker.all.changes.front().problem;
    };
    CHECK(choose(other.file("nothing-here.exe")) == "missing");

    const std::filesystem::path shell = other.file("cmd.exe");
    rv::test::writeProgramStub(shell);
    CHECK(choose(shell) == "refused");

    // A link named like an emulator that opens a shell: the file it opens counts. Without the right to make links here,
    // the translation's own test of the alias stands for it.
    const std::filesystem::path link = other.file("harmless.exe");
    std::error_code linked;
    std::filesystem::create_symlink(shell, link, linked);
    if (!linked) {
        CHECK(choose(link) == "refused");
    }

    const std::filesystem::path good = other.file("standalone.exe");
    rv::test::writeProgramStub(good);
    CHECK(choose(good).empty());
    const rv::ui::EmulatorOption repaired = missingOf(worker.table("gb"));
    CHECK(repaired.readiness == "ready");
    CHECK(repaired.programChosen);
    CHECK(repaired.program == rv::core::pathToUtf8(good));

    // The program gone: not found, and no find rule takes over.
    std::filesystem::remove(good);
    const rv::ui::EmulatorOption gone = missingOf(worker.table("gb"));
    CHECK(gone.readiness == "emulator-not-found");
}

TEST_CASE("app/emulators: the sample: the most recently played, else the first by title", "[app][emulators]") {
    const ImportedFixture fixture("rv-emulators");
    const auto game = fixture.gameAt("gb", "Tiny Tennis (Europe).gb");
    Worker worker(fixture);
    const auto samplesOf = [&] {
        worker.all.emulatorSamples.reset();
        worker.worker.sampleGames(1, kGb, 50);
        worker.wait([&] { return worker.all.emulatorSamples.has_value(); });
        return worker.all.emulatorSamples->games;
    };
    const std::vector<rv::ui::EmulatorSample> first = samplesOf();
    REQUIRE(first.size() >= 2);
    CHECK(first.front().gameId != game.toString()); // by title, nothing played

    PlaySession session;
    session.game = game;
    session.choice = command("Fake Emulator");
    session.outcome = PlaySessionOutcome::Returned;
    session.started = std::chrono::system_clock::now();
    session.ended = session.started;
    session.exitCode = 0;
    session.exitKind = rv::launcher::ExitKind::Success;
    session.owner = rv::catalog::SessionOwner{ImportedFixture::testOwnerProcessId(), 1};
    worker.worker.finishSession(7, session, kGb, {});
    worker.wait([&] { return !worker.all.sessions.empty(); });
    const std::vector<rv::ui::EmulatorSample> played = samplesOf();
    REQUIRE(played.size() == first.size());
    CHECK(played.front().gameId == game.toString());
}

TEST_CASE("app/emulators: what a test's session says", "[app][emulators]") {
    PlaySession session;
    session.outcome = PlaySessionOutcome::Returned;
    CHECK(rv::app::testVerdictOf(session, true) == "verified");
    CHECK(rv::app::testVerdictOf(session, false) == "error");
    session.outcome = PlaySessionOutcome::Crashed;
    CHECK(rv::app::testVerdictOf(session, false) == "crashed");
    session.outcome = PlaySessionOutcome::Blocked;
    CHECK(rv::app::testVerdictOf(session, false) == "blocked");
    session.outcome = PlaySessionOutcome::LeftRunning;
    CHECK(rv::app::testVerdictOf(session, false) == "left-running");
    session.outcome = PlaySessionOutcome::FailedToStart;
    CHECK(rv::app::testVerdictOf(session, false) == "failed");
    session.refusedByGuard = true;
    CHECK(rv::app::testVerdictOf(session, false) == "guard");
    session = PlaySession{};
    session.outcome = PlaySessionOutcome::Returned;
    session.exitCode = 3;
    session.exitKind = rv::launcher::ExitKind::Failure;
    CHECK(rv::app::testVerdictOf(session, false) == "failed");
}
