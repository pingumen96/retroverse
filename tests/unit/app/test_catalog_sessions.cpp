// The play sessions of the app's launches (I2 step 13): what LaunchFlow tells its owner (LaunchFlowEvents: the session
// began, it ended and why) and the play session the ES-DE catalog records for it (startedPlaySession,
// finishedPlaySession, blockedPlaySession: A1's endings mapped as docs/i2/step-12.md says), on a scripted process runner;
// the message of a game that is not ready (refusalOf).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/app_catalog.hpp>
#include <rv/app/launch_flow.hpp>
#include <rv/catalog/play_session.hpp>

#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

using namespace std::chrono_literals;
using rv::app::LaunchEndCause;
using rv::app::LaunchFlow;
using rv::catalog::LeftRunningReason;
using rv::catalog::PlaySession;
using rv::catalog::PlaySessionOutcome;
using rv::launcher::IProcessRunner;
using rv::launcher::IRunningProcess;
using rv::launcher::LaunchClock;
using rv::launcher::LaunchRequest;
using rv::launcher::ProcessEvent;
using rv::launcher::ProcessEventKind;
using rv::launcher::WaitStrategy;

namespace {

constexpr std::uint32_t kProcessId = 4242;

const LaunchClock::time_point kT0 = LaunchClock::time_point{} + 3000h;
LaunchClock::time_point at(int milliseconds) {
    return kT0 + std::chrono::milliseconds(milliseconds);
}
const std::chrono::system_clock::time_point kWallStart = std::chrono::system_clock::time_point{} + 480000h;

class ScriptedProcess final : public IRunningProcess {
public:
    explicit ScriptedProcess(std::vector<ProcessEvent>& queue) : m_queue(queue) {}
    [[nodiscard]] std::uint32_t processId() const noexcept override { return kProcessId; }
    [[nodiscard]] LaunchClock::time_point startedAt() const noexcept override { return kT0; }
    [[nodiscard]] bool followsTree() const noexcept override { return true; }
    void takeEvents(std::vector<ProcessEvent>& out) override {
        out.insert(out.end(), m_queue.begin(), m_queue.end());
        m_queue.clear();
    }
    void stopWatching() noexcept override {}

private:
    std::vector<ProcessEvent>& m_queue;
};

class ScriptedRunner final : public IProcessRunner {
public:
    rv::Expected<std::unique_ptr<IRunningProcess>> start(const LaunchRequest& /*request*/,
                                                         rv::launcher::StartCallbacks /*callbacks*/) override {
        if (failure) {
            return rv::propagate(*failure);
        }
        return std::unique_ptr<IRunningProcess>(std::make_unique<ScriptedProcess>(queue));
    }

    std::vector<ProcessEvent> queue;
    std::optional<rv::Error> failure;
};

ProcessEvent event(ProcessEventKind kind, int milliseconds, std::optional<std::uint32_t> exitCode = std::nullopt) {
    ProcessEvent e;
    e.kind = kind;
    e.occurredAt = at(milliseconds);
    e.observedAt = at(milliseconds + 2);
    e.processId = kProcessId;
    e.exitCode = exitCode;
    return e;
}

/// A flow whose events record the play session as EsdeAppCatalog does.
struct Recorder {
    rv::test::TempDir dir{"rv-catalog-sessions"};
    ScriptedRunner runner;
    LaunchFlow flow{runner};
    PlaySession session;
    std::optional<LaunchEndCause> cause;
    bool stoppedAwaiting = false;
    bool guardAllowed = true;
    int started = 0;

    Recorder() {
        session.game = rv::catalog::GameId::fromValue(0x1234).value();
        session.choice = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::Profile, "fake-return"};
        session.choiceSource = rv::catalog::ChoiceSource::RetroverseSystem;
        session.owner = rv::catalog::SessionOwner{77, 123456789};
        rv::app::LaunchFlowEvents events;
        events.started = [this](const rv::launcher::LaunchSession& launch) {
            ++started;
            session = rv::app::startedPlaySession(session, launch, kWallStart, kT0);
        };
        events.ended = [this](const rv::launcher::LaunchSession& launch, LaunchEndCause why, bool awaiting) {
            cause = why;
            stoppedAwaiting = awaiting;
            session = rv::app::finishedPlaySession(session, launch, why, awaiting, guardAllowed, at(20000));
        };
        flow.setEvents(std::move(events));
    }

    void launch(WaitStrategy strategy = WaitStrategy::DirectProcess) {
        const std::filesystem::path executable = dir.file("emulator.exe");
        if (!std::filesystem::exists(executable)) {
            rv::test::writeProgramStub(executable);
        }
        LaunchRequest request;
        request.executable = executable;
        request.arguments = {"C:\\Giochi\\tennis.gb"};
        request.waitStrategy = strategy;
        request.label = "Tiny Tennis with fake-return";
        REQUIRE(flow.request(std::move(request), rv::app::LaunchInfo{"g0000000000001234", "Tiny Tennis", "fake"}, 8));
        flow.start(kT0, {}, 9);
    }
};

} // namespace

TEST_CASE("app/catalog_sessions: a game that returns is recorded Running, then Returned with its run time",
          "[app][catalog_sessions]") {
    Recorder r;
    r.launch();
    CHECK(r.started == 1);
    CHECK(r.session.outcome == PlaySessionOutcome::Running);
    CHECK(r.session.started == kWallStart);
    CHECK(r.session.processId == kProcessId);
    CHECK_FALSE(r.cause.has_value());

    r.runner.queue.push_back(event(ProcessEventKind::MainExited, 2500, 0));
    r.runner.queue.push_back(event(ProcessEventKind::TreeEmpty, 2500));
    r.flow.poll(at(2510), 9);
    REQUIRE(r.cause == LaunchEndCause::Session);
    CHECK_FALSE(r.stoppedAwaiting);
    CHECK(r.session.outcome == PlaySessionOutcome::Returned);
    CHECK(r.session.exitCode == 0u);
    CHECK(r.session.runTime == 2500ms);
    // The end is the authoritative exit, 2.5 s after the wall-clock start, whenever it was observed.
    CHECK(r.session.ended == kWallStart + 2500ms);
    CHECK_FALSE(r.session.confirmedByUser);
    CHECK(rv::catalog::returnedNormally(r.session));
}

TEST_CASE("app/catalog_sessions: the user's return, quitting and the wait limit leave the game running, each with its "
          "reason",
          "[app][catalog_sessions]") {
    SECTION("Torna a Retroverse while the game runs: the user's confirmation") {
        Recorder r;
        r.launch();
        r.flow.returnToRetroverse(at(4000), 30);
        REQUIRE(r.cause == LaunchEndCause::User);
        CHECK(r.session.outcome == PlaySessionOutcome::LeftRunning);
        CHECK(r.session.leftRunning == LeftRunningReason::Confirmed);
        CHECK(r.session.confirmedByUser);
        CHECK(r.session.runTime == 4000ms);
        CHECK_FALSE(r.session.exitCode.has_value());
        CHECK_FALSE(rv::catalog::returnedNormally(r.session));
    }
    SECTION("quitting Retroverse") {
        Recorder r;
        r.launch();
        r.flow.quit(at(3000), 20);
        REQUIRE(r.cause == LaunchEndCause::Quit);
        CHECK(r.session.outcome == PlaySessionOutcome::LeftRunning);
        CHECK(r.session.leftRunning == LeftRunningReason::Quit);
        CHECK_FALSE(r.session.confirmedByUser);
    }
    SECTION("the wait limit of a frame-limited run") {
        Recorder r;
        r.flow.setWaitLimit(2000ms);
        r.launch();
        r.flow.poll(at(2000), 9);
        REQUIRE(r.cause == LaunchEndCause::WaitLimit);
        CHECK(r.session.outcome == PlaySessionOutcome::LeftRunning);
        CHECK(r.session.leftRunning == LeftRunningReason::WaitLimit);
        CHECK_FALSE(r.session.confirmedByUser);
    }
}

TEST_CASE("app/catalog_sessions: UserConfirmReturn: confirmed it returned; stopped while awaiting it, left running",
          "[app][catalog_sessions]") {
    SECTION("Sono tornato after its launcher's quick exit: a plain, verified return") {
        Recorder r;
        r.launch(WaitStrategy::UserConfirmReturn);
        r.runner.queue.push_back(event(ProcessEventKind::MainExited, 100, 0));
        r.runner.queue.push_back(event(ProcessEventKind::TreeEmpty, 100));
        r.flow.poll(at(150), 9);
        REQUIRE(r.flow.panel() == rv::app::SessionPanel::Confirm);
        r.flow.returnToRetroverse(at(9000), 12);
        REQUIRE(r.cause == LaunchEndCause::User);
        CHECK_FALSE(r.stoppedAwaiting);
        CHECK(r.session.outcome == PlaySessionOutcome::Returned);
        CHECK(r.session.confirmedByUser);
        CHECK(rv::catalog::returnedNormally(r.session));
    }
    SECTION("quitting while the confirmation is awaited") {
        Recorder r;
        r.launch(WaitStrategy::UserConfirmReturn);
        r.runner.queue.push_back(event(ProcessEventKind::MainExited, 100, 0));
        r.runner.queue.push_back(event(ProcessEventKind::TreeEmpty, 100));
        r.flow.poll(at(150), 9);
        r.flow.quit(at(5000), 12);
        REQUIRE(r.cause == LaunchEndCause::Quit);
        CHECK(r.stoppedAwaiting);
        CHECK(r.session.outcome == PlaySessionOutcome::LeftRunning);
        CHECK(r.session.leftRunning == LeftRunningReason::Quit);
        CHECK_FALSE(r.session.quickExit);
        CHECK_FALSE(r.session.confirmedByUser);
    }
}

TEST_CASE("app/catalog_sessions: a launch that fails to start; the automation guard's refusal is told apart",
          "[app][catalog_sessions]") {
    for (const bool allowed : {true, false}) {
        Recorder r;
        r.guardAllowed = allowed;
        r.runner.failure = rv::Error{rv::ErrorCode::PlatformError, "refused", {}};
        r.launch();
        CHECK(r.started == 1);
        REQUIRE(r.cause == LaunchEndCause::Session);
        CHECK(r.session.outcome == PlaySessionOutcome::FailedToStart);
        CHECK(r.session.failure == rv::launcher::FailureReason::StartFailed);
        CHECK(r.session.refusedByGuard == !allowed);
        CHECK_FALSE(r.session.exitCode.has_value());
        CHECK_FALSE(r.session.runTime.has_value());
    }
}

TEST_CASE("app/catalog_sessions: a game that is not ready: a blocked session and the panel's message",
          "[app][catalog_sessions]") {
    rv::app::ResolvedLaunch resolved;
    resolved.resolution.chosen.choice = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::EsdeCommand,
                                                                   "Fake Emulator (Inject)"};
    resolved.resolution.translation.blockers = {
        {rv::catalog::LaunchReadiness::CoreNotFound, "core-not-found", "no core"},
        {rv::catalog::LaunchReadiness::RequiresConfiguration, "inject-file", "ES-DE would inject the arguments"},
    };
    const rv::app::LaunchMessage message = rv::app::refusalOf(resolved);
    CHECK(message.code == rv::app::launch_outcome::kNotReady);
    CHECK(message.reason == "requires-configuration");
    CHECK(message.detail == "ES-DE would inject the arguments");

    PlaySession base;
    base.game = rv::catalog::GameId::fromValue(99).value();
    const PlaySession blocked = rv::app::blockedPlaySession(base, resolved.resolution, kWallStart);
    CHECK(blocked.outcome == PlaySessionOutcome::Blocked);
    CHECK(blocked.readiness == rv::catalog::LaunchReadiness::RequiresConfiguration);
    CHECK(blocked.started == kWallStart);
    CHECK(blocked.ended == kWallStart);
    CHECK(blocked.message == "core-not-found: no core; inject-file: ES-DE would inject the arguments");

    rv::app::ResolvedLaunch unresolved;
    unresolved.error = rv::Error{rv::ErrorCode::NotFound, "the catalog no longer has this game", "g1"};
    const rv::app::LaunchMessage gone = rv::app::refusalOf(unresolved);
    CHECK(gone.code == rv::app::launch_outcome::kNotReady);
    CHECK(gone.reason == "unresolved");
    CHECK(gone.detail == "the catalog no longer has this game");
}

TEST_CASE("app/catalog_sessions: wall-clock times as process creation times count them", "[app][catalog_sessions]") {
    // 1970-01-01 is 11,644,473,600 s after 1601-01-01.
    CHECK(rv::app::fileTimeTicksOf(std::chrono::system_clock::time_point{}) == 116444736000000000ull);
    CHECK(rv::app::fileTimeTicksOf(std::chrono::system_clock::time_point{} + 1s) == 116444736010000000ull);
}

TEST_CASE("app/catalog_sessions: the end causes by name", "[app][catalog_sessions]") {
    CHECK(rv::app::toString(LaunchEndCause::Session) == "session");
    CHECK(rv::app::toString(LaunchEndCause::User) == "user");
    CHECK(rv::app::toString(LaunchEndCause::Quit) == "quit");
    CHECK(rv::app::toString(LaunchEndCause::WaitLimit) == "wait-limit");
}
