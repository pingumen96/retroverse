// A launch from the quick catalog, from Avvia to the return (I2 step A1): rv::app::LaunchFlow around a LaunchSession on
// a scripted process runner. What the host does (idle while the emulator has the screen), what the game-session panel
// shows, what the run report records.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/launch_flow.hpp>

#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

using namespace std::chrono_literals;
using rv::app::LaunchFlow;
using rv::app::LaunchInfo;
using rv::app::SessionPanel;
using rv::launcher::IProcessRunner;
using rv::launcher::IRunningProcess;
using rv::launcher::LaunchClock;
using rv::launcher::LaunchRequest;
using rv::launcher::ProcessEvent;
using rv::launcher::ProcessEventKind;
using rv::launcher::WaitStrategy;

namespace lo = rv::app::launch_outcome;

namespace {

constexpr std::uint32_t kProcessId = 5150;

const LaunchClock::time_point kT0 = LaunchClock::time_point{} + 2000h;
LaunchClock::time_point at(int milliseconds) {
    return kT0 + std::chrono::milliseconds(milliseconds);
}

/// The process a ScriptedRunner starts: hands over the events the test queues.
class ScriptedProcess final : public IRunningProcess {
public:
    ScriptedProcess(std::vector<ProcessEvent>& queue, int& stops) : m_queue(queue), m_stops(stops) {}
    [[nodiscard]] std::uint32_t processId() const noexcept override { return kProcessId; }
    [[nodiscard]] LaunchClock::time_point startedAt() const noexcept override { return kT0; }
    [[nodiscard]] bool followsTree() const noexcept override { return true; }
    void takeEvents(std::vector<ProcessEvent>& out) override {
        out.insert(out.end(), m_queue.begin(), m_queue.end());
        m_queue.clear();
    }
    void stopWatching() noexcept override { ++m_stops; }

private:
    std::vector<ProcessEvent>& m_queue;
    int& m_stops;
};

/// A runner that starts nothing: its process reports what the test queues; the start callbacks run as the real
/// runner runs them (the hand-over before the process runs).
class ScriptedRunner final : public IProcessRunner {
public:
    rv::Expected<std::unique_ptr<IRunningProcess>> start(const LaunchRequest& /*request*/,
                                                         rv::launcher::StartCallbacks callbacks) override {
        ++starts;
        if (failure) {
            return rv::propagate(*failure);
        }
        if (callbacks.beforeResume) {
            callbacks.beforeResume(kProcessId);
        }
        wake = callbacks.wake;
        return std::unique_ptr<IRunningProcess>(std::make_unique<ScriptedProcess>(queue, stops));
    }

    std::vector<ProcessEvent> queue;
    std::optional<rv::Error> failure;
    std::function<void()> wake;
    int starts = 0;
    int stops = 0;
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

struct Fixture {
    rv::test::TempDir dir{"rv-launch-flow"};
    ScriptedRunner runner;
    LaunchFlow flow{runner};

    LaunchRequest request(WaitStrategy strategy = WaitStrategy::DirectProcess) {
        const std::filesystem::path executable = dir.file("emulator.exe");
        if (!std::filesystem::exists(executable)) {
            rv::test::writeProgramStub(executable);
        }
        LaunchRequest r;
        r.executable = executable;
        r.arguments = {"C:\\Giochi\\lanterna.gb"};
        r.waitStrategy = strategy;
        r.label = "Lanterna di Vetro with Emulatore di prova";
        return r;
    }

    static LaunchInfo info() { return LaunchInfo{"g000000000000a001", "Lanterna di Vetro", "fake"}; }

    /// Avvia on frame 8, the frame presented, the start on the host's first wake (frame index 9).
    void launch(WaitStrategy strategy = WaitStrategy::DirectProcess) {
        REQUIRE(flow.request(request(strategy), info(), 8));
        REQUIRE(flow.pending());
        CHECK(flow.hostIdle());
        CHECK(flow.panel() == SessionPanel::Launching);
        bool handedOver = false;
        rv::launcher::StartCallbacks callbacks;
        callbacks.beforeResume = [&handedOver](std::uint32_t) { handedOver = true; };
        callbacks.wake = [] {};
        flow.start(kT0, std::move(callbacks), 9);
        CHECK(handedOver);
        CHECK(runner.starts == 1);
    }
};

} // namespace

TEST_CASE("app/launch_flow: the host waits while the emulator runs and the frames resume at its return",
          "[app][launch_flow]") {
    Fixture f;
    f.launch();
    REQUIRE(f.flow.sessionActive());
    CHECK_FALSE(f.flow.pending());
    // The Launching frame ("Gioco in corso") was presented and stays behind the emulator: nothing to ask, the host
    // waits.
    CHECK(f.flow.panel() == SessionPanel::Launching);
    CHECK(f.flow.hostIdle());
    REQUIRE(f.runner.wake); // the host's wake event reaches the runner

    // Wakes while it runs: counted, nothing changes.
    f.flow.idleWake();
    f.flow.poll(at(500), 9);
    f.flow.idleWake();
    CHECK(f.flow.sessionActive());
    CHECK(f.flow.hostIdle());
    CHECK_FALSE(f.flow.takeReturned());

    // The emulator exits after 2.5 s: the session ends, the window comes back, nothing to tell.
    f.runner.queue.push_back(event(ProcessEventKind::MainExited, 2500, 0));
    f.flow.poll(at(2510), 9);
    CHECK_FALSE(f.flow.sessionActive());
    CHECK_FALSE(f.flow.hostIdle());
    CHECK(f.flow.panel() == SessionPanel::None);
    CHECK(f.flow.takeReturned());
    CHECK_FALSE(f.flow.takeReturned()); // once
    CHECK(f.flow.message().code == lo::kReturned);
    CHECK(f.runner.stops == 1); // the session stopped following the process; nothing was ended

    REQUIRE(f.flow.records().size() == 1);
    const rv::app::LaunchRecord& record = f.flow.records().front();
    CHECK(record.gameId == "g000000000000a001");
    CHECK(record.profile == "fake");
    CHECK(record.state == "Returned");
    CHECK(record.outcome == lo::kReturned);
    CHECK(record.exitCode == 0u);
    CHECK(record.strategy == "DirectProcess");
    CHECK(record.processId == kProcessId);
    CHECK_FALSE(record.quickExit);
    CHECK(record.requestFrame == 8);
    CHECK(record.startFrame == 9u);
    CHECK(record.endFrame == 9u);
    CHECK(record.framesWhileRunning == 0);
    CHECK(record.framesWithPanel == 0);
    CHECK(record.idleWakes == 2);
    REQUIRE(record.runMs.has_value());
    CHECK(*record.runMs == 2500.0);
    CHECK(record.panels.empty());

    // A frame rendered after the return counts for nothing.
    f.flow.frameRendered();
    CHECK(f.flow.records().front().framesWhileRunning == 0);
}

TEST_CASE("app/launch_flow: one launch at a time", "[app][launch_flow]") {
    Fixture f;
    f.launch();
    CHECK_FALSE(f.flow.request(f.request(), Fixture::info(), 10));
    CHECK_FALSE(f.flow.refuse(rv::app::LaunchMessage{std::string(lo::kRomMissing), "", "x.gb"}, Fixture::info(), 10));
    CHECK(f.flow.records().size() == 1);
    // A start without a pending request does nothing.
    f.flow.start(at(10), {}, 9);
    CHECK(f.runner.starts == 1);
}

TEST_CASE("app/launch_flow: endings with something to tell show the outcome panel", "[app][launch_flow]") {
    SECTION("a crash") {
        Fixture f;
        f.launch();
        f.runner.queue.push_back(event(ProcessEventKind::MainExited, 300, 0xC0000005u));
        f.flow.poll(at(310), 9);
        CHECK(f.flow.panel() == SessionPanel::Outcome);
        CHECK(f.flow.message().code == lo::kCrashed);
        CHECK(f.flow.message().detail == "0xC0000005 STATUS_ACCESS_VIOLATION");
        CHECK_FALSE(f.flow.hostIdle()); // the panel renders
        CHECK(f.flow.records().front().panels == std::vector<std::string>{"outcome"});
        // Dismissed (OK, or Back): the catalog again.
        CHECK_FALSE(f.flow.panelBack(at(400)));
        CHECK(f.flow.panel() == SessionPanel::None);
    }
    SECTION("an error code at once: a failure to start") {
        Fixture f;
        f.launch();
        f.runner.queue.push_back(event(ProcessEventKind::MainExited, 100, 3));
        f.flow.poll(at(120), 9);
        CHECK(f.flow.message().code == lo::kFailedToStart);
        CHECK(f.flow.message().reason == lo::kReasonEarlyExit);
        CHECK(f.flow.records().front().state == "FailedToStart");
        f.flow.dismissOutcome();
        CHECK(f.flow.panel() == SessionPanel::None);
    }
    SECTION("a clean exit at once: a quick exit, with the hint") {
        Fixture f;
        f.launch();
        f.runner.queue.push_back(event(ProcessEventKind::MainExited, 100, 0));
        f.flow.poll(at(120), 9);
        CHECK(f.flow.message().code == lo::kQuickExit);
        CHECK(f.flow.records().front().quickExit);
        CHECK(f.flow.panel() == SessionPanel::Outcome);
    }
    SECTION("the runner refuses (the automation guard): failed to start with its reason, no wait") {
        Fixture f;
        f.runner.failure = rv::Error{rv::ErrorCode::ValidationFailed, "not on the allow-list", "RV_LAUNCH_ALLOWLIST"};
        REQUIRE(f.flow.request(f.request(), Fixture::info(), 8));
        f.flow.start(kT0, {}, 9);
        CHECK_FALSE(f.flow.sessionActive());
        CHECK_FALSE(f.flow.hostIdle());
        CHECK(f.flow.takeReturned());
        CHECK(f.flow.panel() == SessionPanel::Outcome);
        CHECK(f.flow.message().code == lo::kFailedToStart);
        CHECK(f.flow.message().reason == lo::kReasonGuardRefused);
        CHECK(f.flow.message().detail.find("not on the allow-list") != std::string::npos);
        CHECK(f.flow.records().front().reason == lo::kReasonGuardRefused);
        CHECK(f.flow.records().front().endFrame == 9u);
    }
    SECTION("the runner fails otherwise: failed to start, the runner's error") {
        Fixture f;
        f.runner.failure = rv::Error{rv::ErrorCode::PlatformError, "CreateProcessW failed: access denied", "x"};
        REQUIRE(f.flow.request(f.request(), Fixture::info(), 8));
        f.flow.start(kT0, {}, 9);
        CHECK(f.flow.panel() == SessionPanel::Outcome);
        CHECK(f.flow.message().code == lo::kFailedToStart);
        CHECK(f.flow.message().reason == lo::kReasonStartFailed);
        CHECK(f.flow.message().detail.find("access denied") != std::string::npos);
    }
}

TEST_CASE("app/launch_flow: a launch that cannot start shows its message at once", "[app][launch_flow]") {
    Fixture f;
    const rv::app::LaunchMessage message{std::string(lo::kNotConfigured), std::string(lo::kReasonTemplate),
                                         "{var:core}"};
    REQUIRE(f.flow.refuse(message, Fixture::info(), 12));
    CHECK(f.runner.starts == 0);
    CHECK(f.flow.panel() == SessionPanel::Outcome);
    CHECK(f.flow.message() == message);
    CHECK_FALSE(f.flow.hostIdle());
    REQUIRE(f.flow.records().size() == 1);
    CHECK(f.flow.records().front().state == "Blocked");
    CHECK(f.flow.records().front().outcome == lo::kNotConfigured);
    CHECK(f.flow.records().front().detail == "{var:core}");
    CHECK(f.flow.records().front().requestFrame == 12);
    CHECK_FALSE(f.flow.panelBack(at(100)));
    CHECK(f.flow.panel() == SessionPanel::None);
    // The next launch is possible.
    CHECK(f.flow.request(f.request(), Fixture::info(), 20));
}

TEST_CASE("app/launch_flow: StillRunning asks the user, who returns leaving the game running", "[app][launch_flow]") {
    Fixture f;
    LaunchRequest r = f.request();
    r.timeouts.windowTimeout = 1000ms;
    REQUIRE(f.flow.request(r, Fixture::info(), 8));
    f.flow.start(kT0, {}, 9);
    f.flow.poll(at(500), 9);
    CHECK(f.flow.hostIdle());
    // No window for the window timeout: the host stops waiting to show the question.
    f.flow.poll(at(1100), 9);
    CHECK(f.flow.panel() == SessionPanel::StillRunning);
    CHECK_FALSE(f.flow.hostIdle());
    // The panel's frames count apart from the emulator's.
    f.flow.frameRendered();
    f.flow.frameRendered();
    // "Continua ad aspettare": a frame with the notice, then the host waits again; the game's window resumes Running,
    // a new timeout asks again.
    CHECK_FALSE(f.flow.backToGame(at(1200))); // the window does not have the focus: nothing to hand over
    CHECK(f.flow.panel() == SessionPanel::Launching);
    CHECK(f.flow.hostIdle());
    f.flow.frameRendered(); // the frame of the notice: the game has the screen again
    f.runner.queue.push_back(event(ProcessEventKind::WindowShown, 1500));
    f.runner.queue.push_back(event(ProcessEventKind::WindowsGone, 2000));
    f.flow.poll(at(2100), 11);
    CHECK(f.flow.panel() == SessionPanel::Launching);
    f.flow.poll(at(3100), 11);
    CHECK(f.flow.panel() == SessionPanel::StillRunning);
    // "Torna a Retroverse": the session ends, the emulator left running, nothing to tell.
    f.flow.returnToRetroverse(at(3200), 13);
    CHECK_FALSE(f.flow.sessionActive());
    CHECK(f.flow.takeReturned());
    CHECK(f.flow.panel() == SessionPanel::None);
    CHECK(f.flow.message().code == lo::kLeftRunning);
    CHECK(f.flow.message().reason == lo::kReasonConfirmed);
    const rv::app::LaunchRecord& record = f.flow.records().front();
    CHECK(record.state == "Returned");
    CHECK(record.processesRemained);
    CHECK(record.confirmedByUser);
    CHECK(record.wasStillRunning);
    CHECK_FALSE(record.quitDuringGame);
    CHECK(record.framesWithPanel == 2);
    CHECK(record.framesWhileRunning == 1);
    CHECK(record.startFrame == 9u);
    CHECK(record.endFrame == 13u);
    CHECK(record.panels == std::vector<std::string>{"still-running", "still-running"});
    CHECK(f.runner.stops == 1);
}

TEST_CASE("app/launch_flow: StillRunning ends by itself when the game shows a window again", "[app][launch_flow]") {
    Fixture f;
    LaunchRequest r = f.request();
    r.timeouts.windowTimeout = 1000ms;
    REQUIRE(f.flow.request(r, Fixture::info(), 8));
    f.flow.start(kT0, {}, 9);
    f.flow.poll(at(1100), 9);
    REQUIRE(f.flow.panel() == SessionPanel::StillRunning);
    f.runner.queue.push_back(event(ProcessEventKind::WindowShown, 1200));
    f.flow.poll(at(1210), 10);
    CHECK(f.flow.panel() == SessionPanel::Launching);
    CHECK(f.flow.hostIdle());
}

TEST_CASE("app/launch_flow: UserConfirmReturn asks for the confirmation once the started program ended",
          "[app][launch_flow]") {
    Fixture f;
    f.launch(WaitStrategy::UserConfirmReturn);
    f.runner.queue.push_back(event(ProcessEventKind::MainExited, 2500, 0));
    f.flow.poll(at(2510), 9);
    CHECK(f.flow.sessionActive());
    CHECK(f.flow.panel() == SessionPanel::Confirm);
    CHECK_FALSE(f.flow.hostIdle());
    // Back on the panel: back to the game, the host waits again (the confirmation is still owed).
    CHECK_FALSE(f.flow.panelBack(at(2600)));
    CHECK(f.flow.panel() == SessionPanel::Launching);
    CHECK(f.flow.hostIdle());
    // The user comes back to Retroverse and stays: asked again, after the delay.
    f.flow.focusChanged(true, at(4000));
    f.flow.poll(at(4000 + 1000), 9);
    CHECK(f.flow.panel() == SessionPanel::Launching);
    f.flow.poll(at(4000 + 1500), 9);
    CHECK(f.flow.panel() == SessionPanel::Confirm);
    // "Sono tornato".
    f.flow.returnToRetroverse(at(6000), 12);
    CHECK_FALSE(f.flow.sessionActive());
    CHECK(f.flow.message().code == lo::kReturned);
    CHECK(f.flow.panel() == SessionPanel::None);
    CHECK(f.flow.records().front().confirmedByUser);
    CHECK(f.flow.records().front().panels == std::vector<std::string>{"confirm", "confirm"});
}

TEST_CASE("app/launch_flow: the user back in Retroverse while the game runs is asked, after a delay",
          "[app][launch_flow]") {
    Fixture f;
    f.launch();
    // The emulator's window closing hands the focus back before its process ends: no panel within the delay.
    f.flow.focusChanged(true, at(5000));
    f.flow.poll(at(5300), 9);
    CHECK(f.flow.panel() == SessionPanel::Launching);
    f.runner.queue.push_back(event(ProcessEventKind::MainExited, 5350, 0));
    f.flow.poll(at(5400), 9);
    CHECK_FALSE(f.flow.sessionActive());
    CHECK(f.flow.records().front().panels.empty());

    // A user who switches to Retroverse and stays is asked; losing the focus first cancels.
    Fixture g;
    g.launch();
    g.flow.focusChanged(true, at(1000));
    g.flow.focusChanged(false, at(1200));
    g.flow.poll(at(3000), 9);
    CHECK(g.flow.panel() == SessionPanel::Launching);
    g.flow.focusChanged(true, at(3000));
    g.flow.poll(at(3000 + 1500), 9);
    CHECK(g.flow.panel() == SessionPanel::Playing);
    CHECK_FALSE(g.flow.hostIdle());
    CHECK(g.flow.backToGame(at(4600))); // focused: the application hands the foreground to the game
    CHECK(g.flow.hostIdle());
    g.flow.focusChanged(false, at(4650)); // the game took it
    // Asked again, the user switches back to the game without answering: the game has the screen again.
    g.flow.focusChanged(true, at(5000));
    g.flow.poll(at(6600), 9);
    REQUIRE(g.flow.panel() == SessionPanel::Playing);
    g.flow.focusChanged(false, at(6700));
    CHECK(g.flow.panel() == SessionPanel::Launching);
    CHECK(g.flow.hostIdle());
    // Not asked again until the user comes back once more.
    g.flow.poll(at(9000), 9);
    CHECK(g.flow.panel() == SessionPanel::Launching);
    // The emulator exits while the user looks at the panel: the return as usual.
    g.flow.focusChanged(true, at(9100));
    g.flow.poll(at(10700), 9);
    REQUIRE(g.flow.panel() == SessionPanel::Playing);
    g.runner.queue.push_back(event(ProcessEventKind::MainExited, 11000, 0));
    g.flow.poll(at(11010), 15);
    CHECK_FALSE(g.flow.sessionActive());
    CHECK(g.flow.panel() == SessionPanel::None);
    CHECK(g.flow.records().front().endFrame == 15u);
}

TEST_CASE("app/launch_flow: a question nobody can see is presented once, then waits for the focus",
          "[app][launch_flow]") {
    // UserConfirmReturn: the started program (a launcher) ended while the game runs in front of Retroverse.
    Fixture f;
    f.flow.focusChanged(true, kT0);
    f.launch(WaitStrategy::UserConfirmReturn);
    f.flow.focusChanged(false, at(300)); // the game took the foreground
    f.runner.queue.push_back(event(ProcessEventKind::MainExited, 2500, 0));
    f.flow.poll(at(2510), 9);
    REQUIRE(f.flow.panel() == SessionPanel::Confirm);
    // Its frame is rendered once, the frame the window keeps behind the game; then no more GPU work.
    CHECK_FALSE(f.flow.hostIdle());
    f.flow.frameRendered();
    CHECK(f.flow.hostIdle());
    // The user comes back: the question renders, at once (no come-back delay: it was asked already).
    f.flow.focusChanged(true, at(60000));
    CHECK_FALSE(f.flow.hostIdle());
    f.flow.poll(at(60000), 10);
    CHECK(f.flow.panel() == SessionPanel::Confirm);
    f.flow.frameRendered();
    // Away again without answering: the question stays, presented, and the host waits.
    f.flow.focusChanged(false, at(61000));
    CHECK(f.flow.panel() == SessionPanel::Confirm);
    CHECK(f.flow.hostIdle());
    CHECK(f.flow.records().front().framesWithPanel == 2);

    // StillRunning alike.
    Fixture g;
    LaunchRequest r = g.request();
    r.timeouts.windowTimeout = 1000ms;
    REQUIRE(g.flow.request(r, Fixture::info(), 8));
    g.flow.start(kT0, {}, 9);
    g.flow.poll(at(1100), 9);
    REQUIRE(g.flow.panel() == SessionPanel::StillRunning);
    CHECK_FALSE(g.flow.hostIdle());
    g.flow.frameRendered();
    CHECK(g.flow.hostIdle());
    g.flow.focusChanged(true, at(5000));
    CHECK_FALSE(g.flow.hostIdle());
}

TEST_CASE("app/launch_flow: back to the game from the focused window hands it the foreground, never a dead end",
          "[app][launch_flow]") {
    SECTION("Confirm: the question comes back after the delay when the game never takes the focus, or at a press") {
        Fixture f;
        f.flow.focusChanged(true, kT0);
        f.launch(WaitStrategy::UserConfirmReturn);
        f.runner.queue.push_back(event(ProcessEventKind::MainExited, 2500, 0));
        f.flow.poll(at(2510), 9);
        REQUIRE(f.flow.panel() == SessionPanel::Confirm);
        // Back on the panel ("Torna al gioco"): the application must hand the foreground to the game.
        CHECK(f.flow.panelBack(at(3000)));
        CHECK(f.flow.panel() == SessionPanel::Launching);
        CHECK(f.flow.hostIdle());
        // The window kept the focus (nothing took it): asked again once the come-back delay passed.
        f.flow.poll(at(3000 + 1000), 10);
        CHECK(f.flow.panel() == SessionPanel::Launching);
        f.flow.poll(at(3000 + 1500), 10);
        CHECK(f.flow.panel() == SessionPanel::Confirm);
        CHECK_FALSE(f.flow.hostIdle());
        // A press in the window asks again at once.
        CHECK(f.flow.backToGame(at(5000)));
        CHECK(f.flow.userPressed());
        CHECK(f.flow.panel() == SessionPanel::Confirm);
        // The game takes the focus: no press reaches the window, nothing asks until the user comes back.
        CHECK(f.flow.backToGame(at(6000)));
        f.flow.focusChanged(false, at(6100));
        CHECK_FALSE(f.flow.userPressed());
        f.flow.poll(at(20000), 10);
        CHECK(f.flow.panel() == SessionPanel::Launching);
        f.flow.focusChanged(true, at(30000));
        CHECK_FALSE(f.flow.userPressed()); // a gain alone: the come-back delay first
        f.flow.poll(at(31500), 10);
        CHECK(f.flow.panel() == SessionPanel::Confirm);
        CHECK(f.flow.records().front().panels == std::vector<std::string>{"confirm", "confirm", "confirm", "confirm"});
    }
    SECTION("Playing: asked again when the game never takes the focus") {
        Fixture f;
        f.launch();
        f.flow.focusChanged(true, at(1000));
        f.flow.poll(at(2500), 9);
        REQUIRE(f.flow.panel() == SessionPanel::Playing);
        CHECK(f.flow.backToGame(at(2600)));
        f.flow.poll(at(4000), 10);
        CHECK(f.flow.panel() == SessionPanel::Launching);
        f.flow.poll(at(4100), 10);
        CHECK(f.flow.panel() == SessionPanel::Playing);
    }
    SECTION("StillRunning: keep waiting in front of Retroverse; a press asks again") {
        Fixture f;
        f.flow.focusChanged(true, kT0);
        LaunchRequest r = f.request();
        r.timeouts.windowTimeout = 1000ms;
        REQUIRE(f.flow.request(r, Fixture::info(), 8));
        f.flow.start(kT0, {}, 9);
        f.flow.poll(at(1100), 9);
        REQUIRE(f.flow.panel() == SessionPanel::StillRunning);
        // The game shows no window to hand the foreground to.
        CHECK_FALSE(f.flow.backToGame(at(1200)));
        CHECK(f.flow.hostIdle());
        f.flow.poll(at(9000), 10);
        CHECK(f.flow.panel() == SessionPanel::Launching); // the user chose to wait: no timer
        CHECK(f.flow.userPressed());
        CHECK(f.flow.panel() == SessionPanel::StillRunning);
    }
    SECTION("from a window without the focus: nothing to hand over, a press asks nothing") {
        Fixture f;
        f.launch(WaitStrategy::UserConfirmReturn);
        f.runner.queue.push_back(event(ProcessEventKind::MainExited, 2500, 0));
        f.flow.poll(at(2510), 9);
        REQUIRE(f.flow.panel() == SessionPanel::Confirm);
        CHECK_FALSE(f.flow.backToGame(at(3000)));
        CHECK_FALSE(f.flow.userPressed());
        f.flow.poll(at(9000), 10);
        CHECK(f.flow.panel() == SessionPanel::Launching);
    }
}

TEST_CASE("app/launch_flow: processes of the tree left running are told, and the window stays where it is",
          "[app][launch_flow]") {
    Fixture f;
    f.launch();
    // A wrapper started the emulator and exits after the startup grace: the emulator, its child, still runs.
    ProcessEvent child = event(ProcessEventKind::ChildStarted, 400);
    child.processId = kProcessId + 1;
    child.image = "emulator.exe";
    f.runner.queue.push_back(child);
    f.runner.queue.push_back(event(ProcessEventKind::MainExited, 3000, 0));
    f.flow.poll(at(3010), 9);
    CHECK_FALSE(f.flow.sessionActive());
    CHECK(f.flow.panel() == SessionPanel::Outcome);
    CHECK(f.flow.message().code == lo::kProcessesRemained);
    CHECK(f.flow.takeReturned());
    CHECK_FALSE(f.flow.returnTakesForeground()); // never raised over a game that may still run
    const rv::app::LaunchRecord& record = f.flow.records().front();
    CHECK(record.state == "Returned");
    CHECK(record.outcome == lo::kProcessesRemained);
    CHECK(record.processesRemained);
    CHECK_FALSE(record.quickExit);
    CHECK_FALSE(record.confirmedByUser);

    // A plain return takes the foreground back.
    Fixture g;
    g.launch();
    g.runner.queue.push_back(event(ProcessEventKind::MainExited, 3000, 0));
    g.flow.poll(at(3010), 9);
    CHECK(g.flow.takeReturned());
    CHECK(g.flow.returnTakesForeground());
}

TEST_CASE("app/launch_flow: UserConfirmReturn confirmed after a quick exit of its launcher is a plain return",
          "[app][launch_flow]") {
    Fixture f;
    f.flow.focusChanged(true, kT0);
    f.launch(WaitStrategy::UserConfirmReturn);
    // The started program exits at once, as the launchers this strategy is for do.
    f.runner.queue.push_back(event(ProcessEventKind::MainExited, 100, 0));
    f.flow.poll(at(120), 9);
    REQUIRE(f.flow.panel() == SessionPanel::Confirm);
    f.flow.returnToRetroverse(at(90000), 10);
    CHECK_FALSE(f.flow.sessionActive());
    CHECK(f.flow.message().code == lo::kReturned);
    CHECK(f.flow.panel() == SessionPanel::None);
    CHECK(f.flow.records().front().quickExit);
    CHECK(f.flow.records().front().confirmedByUser);
}

TEST_CASE("app/launch_flow: a frame-limited run stops waiting after its wait limit, the game left running",
          "[app][launch_flow]") {
    Fixture f;
    f.flow.setWaitLimit(2000ms);
    f.launch();
    f.flow.poll(at(1900), 9);
    CHECK(f.flow.sessionActive());
    f.flow.poll(at(2000), 9);
    CHECK_FALSE(f.flow.sessionActive());
    CHECK(f.runner.stops == 1); // followed no more, never ended
    CHECK(f.flow.message().code == lo::kLeftRunning);
    CHECK(f.flow.message().reason == lo::kReasonWaitLimit);
    CHECK(f.flow.panel() == SessionPanel::Outcome);
    CHECK(f.flow.takeReturned());
    CHECK_FALSE(f.flow.returnTakesForeground());
    const rv::app::LaunchRecord& record = f.flow.records().front();
    CHECK(record.waitLimitReached);
    CHECK(record.processesRemained);
    CHECK_FALSE(record.confirmedByUser); // nobody confirmed
    CHECK_FALSE(record.quitDuringGame);
    // The next launch has its own limit.
    f.flow.dismissOutcome();
    REQUIRE(f.flow.request(f.request(), Fixture::info(), 20));
    f.flow.start(at(10000), {}, 21);
    f.flow.poll(at(11000), 21);
    CHECK(f.flow.sessionActive());
    CHECK_FALSE(f.flow.records().back().waitLimitReached);
}

TEST_CASE("app/launch_flow: quitting while the game runs leaves it running", "[app][launch_flow]") {
    Fixture f;
    f.launch();
    f.flow.poll(at(1000), 9);
    f.flow.quit(at(1500), 9);
    CHECK_FALSE(f.flow.sessionActive());
    CHECK(f.flow.panel() == SessionPanel::None); // the run is ending: nothing shows
    CHECK(f.runner.stops == 1);                  // followed no more, never ended
    const rv::app::LaunchRecord& record = f.flow.records().front();
    CHECK(record.state == "Returned");
    CHECK(record.outcome == lo::kLeftRunning);
    CHECK(record.reason == lo::kReasonQuit);
    CHECK(record.quitDuringGame);
    CHECK(record.processesRemained);
    CHECK_FALSE(record.confirmedByUser);
    CHECK(record.startFrame == record.endFrame);

    // Quitting before a pending launch started: it never starts.
    Fixture g;
    REQUIRE(g.flow.request(g.request(), Fixture::info(), 8));
    g.flow.quit(kT0, 9);
    CHECK_FALSE(g.flow.pending());
    CHECK(g.runner.starts == 0);
    CHECK(g.flow.records().front().state == "Pending");
    // Quitting with nothing running changes nothing.
    Fixture h;
    h.flow.quit(kT0, 0);
    CHECK(h.flow.records().empty());
}

TEST_CASE("app/launch_flow: the panels by name", "[app][launch_flow]") {
    CHECK(rv::app::toString(SessionPanel::None) == "none");
    CHECK(rv::app::toString(SessionPanel::Launching) == "launching");
    CHECK(rv::app::toString(SessionPanel::Playing) == "playing");
    CHECK(rv::app::toString(SessionPanel::StillRunning) == "still-running");
    CHECK(rv::app::toString(SessionPanel::Confirm) == "confirm");
    CHECK(rv::app::toString(SessionPanel::Outcome) == "outcome");
}
