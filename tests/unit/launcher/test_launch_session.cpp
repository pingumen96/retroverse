#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <rv/launcher/launch_session.hpp>

#include "support/program_stub.hpp"
#include "support/temp_dir.hpp"

using namespace std::chrono_literals;
using rv::launcher::ExitKind;
using rv::launcher::FailureReason;
using rv::launcher::IProcessRunner;
using rv::launcher::IRunningProcess;
using rv::launcher::LaunchClock;
using rv::launcher::LaunchRequest;
using rv::launcher::LaunchSession;
using rv::launcher::ProcessEvent;
using rv::launcher::ProcessEventKind;
using rv::launcher::SessionState;
using rv::launcher::WaitStrategy;

namespace {

constexpr std::uint32_t kMainId = 4242;
constexpr std::uint32_t kChildId = 77;

/// Time points of a script: milliseconds after the start.
const LaunchClock::time_point kT0 = LaunchClock::time_point{} + 1000h;
LaunchClock::time_point at(int milliseconds) {
    return kT0 + std::chrono::milliseconds(milliseconds);
}

/// The process a FakeRunner "starts": hands over whatever the test queued in the runner.
class FakeProcess final : public IRunningProcess {
public:
    FakeProcess(std::vector<ProcessEvent>& queue, bool followsTree, LaunchClock::time_point startedAt, int& stops)
        : m_queue(queue), m_followsTree(followsTree), m_startedAt(startedAt), m_stops(stops) {}
    [[nodiscard]] std::uint32_t processId() const noexcept override { return kMainId; }
    [[nodiscard]] LaunchClock::time_point startedAt() const noexcept override { return m_startedAt; }
    [[nodiscard]] bool followsTree() const noexcept override { return m_followsTree; }
    void takeEvents(std::vector<ProcessEvent>& out) override {
        out.insert(out.end(), m_queue.begin(), m_queue.end());
        m_queue.clear();
    }
    void stopWatching() noexcept override { ++m_stops; }

private:
    std::vector<ProcessEvent>& m_queue;
    bool m_followsTree;
    LaunchClock::time_point m_startedAt;
    int& m_stops;
};

/// A runner whose processes report the events the test scripts.
class FakeRunner final : public IProcessRunner {
public:
    rv::Expected<std::unique_ptr<IRunningProcess>> start(const LaunchRequest& request,
                                                         rv::launcher::StartCallbacks callbacks) override {
        ++starts;
        lastRequest = request;
        if (failure) {
            return rv::propagate(*failure);
        }
        if (throwOnStart) {
            throw std::runtime_error("the runner broke its promise");
        }
        if (callbacks.beforeResume) {
            callbacks.beforeResume(kMainId);
        }
        return std::unique_ptr<IRunningProcess>(std::make_unique<FakeProcess>(queue, followTree, startedAt, stops));
    }

    std::vector<ProcessEvent> queue;
    bool followTree = true;
    /// When the "process" started running (after the window's hand-over).
    LaunchClock::time_point startedAt = kT0;
    std::optional<rv::Error> failure;
    bool throwOnStart = false;
    int starts = 0;
    /// Calls of stopWatching() on the started process.
    int stops = 0;
    std::optional<LaunchRequest> lastRequest;
};

ProcessEvent event(ProcessEventKind kind, int milliseconds, std::uint32_t processId = kMainId,
                   std::optional<std::uint32_t> exitCode = std::nullopt) {
    ProcessEvent e;
    e.kind = kind;
    e.occurredAt = at(milliseconds);
    e.observedAt = at(milliseconds + 1); // noticed a millisecond later
    e.processId = processId;
    e.exitCode = exitCode;
    return e;
}

ProcessEvent mainExited(int milliseconds, std::uint32_t code) {
    return event(ProcessEventKind::MainExited, milliseconds, kMainId, code);
}

/// A request for an emulator.exe that passes validation (its headers only: the fake runner starts nothing).
struct Fixture {
    rv::test::TempDir dir{"rv-launch-session"};
    FakeRunner runner;

    LaunchRequest request(WaitStrategy strategy = WaitStrategy::DirectProcess) {
        const std::filesystem::path executable = dir.file("emulator.exe");
        if (!std::filesystem::exists(executable)) {
            rv::test::writeProgramStub(executable);
        }
        LaunchRequest r;
        r.executable = executable;
        r.arguments = {"--fullscreen", "C:\\Games\\a game.rom"};
        r.waitStrategy = strategy;
        r.label = "Test emulator";
        return r;
    }

    /// The session with `events` delivered after a start at kT0, polled at `pollAt`.
    LaunchSession run(const LaunchRequest& r, std::vector<ProcessEvent> events, int pollAt) {
        LaunchSession session(r, runner);
        session.start(kT0);
        runner.queue = std::move(events);
        session.poll(at(pollAt));
        return session;
    }
};

} // namespace

TEST_CASE("launcher/launch_session: a direct process that exits returns", "[launcher][launch_session]") {
    Fixture f;
    LaunchSession session(f.request(), f.runner);
    CHECK(session.state() == SessionState::Idle);
    session.start(kT0);
    REQUIRE(session.state() == SessionState::Running);
    CHECK(f.runner.starts == 1);
    CHECK(session.processId() == kMainId);
    CHECK(session.commandLine().find("\"C:\\Games\\a game.rom\"") != std::string::npos);
    CHECK(session.liveProcessCount() == 1);

    session.poll(at(4000));
    CHECK(session.state() == SessionState::Running); // nothing happened yet

    f.runner.queue.push_back(event(ProcessEventKind::WindowShown, 900));
    f.runner.queue.push_back(mainExited(5000, 0));
    session.poll(at(5002));
    REQUIRE(session.state() == SessionState::Returned);
    const auto& outcome = session.outcome();
    CHECK(outcome.state == SessionState::Returned);
    CHECK(outcome.exitCode == 0u);
    CHECK(outcome.exitKind == ExitKind::Success);
    CHECK(outcome.exitProcessId == kMainId);
    CHECK_FALSE(outcome.quickExit);
    CHECK_FALSE(outcome.processesRemained);
    CHECK(outcome.processesSeen == 1);
    CHECK(session.times().started == at(0));
    CHECK(session.times().firstWindow == at(901));
    CHECK(session.times().exited == at(5000));
    CHECK(session.times().exitObserved == at(5001));
    CHECK(session.times().ended == at(5001));
    CHECK(session.history().size() == 2);

    // A finished session ignores what comes after.
    f.runner.queue.push_back(mainExited(6000, 9));
    session.poll(at(6001));
    session.confirmReturn(at(6002));
    CHECK(session.outcome().exitCode == 0u);
    session.start(at(7000));
    CHECK(f.runner.starts == 1);
}

TEST_CASE("launcher/launch_session: exits within the startup grace", "[launcher][launch_session]") {
    Fixture f;
    SECTION("an unclean one is a failure to start") {
        const LaunchSession session = f.run(f.request(), {mainExited(300, 1)}, 301);
        CHECK(session.state() == SessionState::FailedToStart);
        CHECK(session.outcome().failure == FailureReason::EarlyExit);
        CHECK(session.outcome().exitCode == 1u);
    }
    SECTION("a clean one returns as a quick exit, a hint at a launcher") {
        const LaunchSession session = f.run(f.request(), {mainExited(300, 0)}, 301);
        CHECK(session.state() == SessionState::Returned);
        CHECK(session.outcome().quickExit);
        CHECK(session.outcome().message.find("launcher") != std::string::npos);
    }
    SECTION("the grace can be turned off") {
        LaunchRequest request = f.request();
        request.timeouts.startupGrace = 0ms;
        const LaunchSession session = f.run(request, {mainExited(300, 1)}, 301);
        CHECK(session.state() == SessionState::Returned);
        CHECK(session.outcome().exitKind == ExitKind::ErrorCode);
    }
    SECTION("after the grace an error code still returns") {
        const LaunchSession session = f.run(f.request(), {mainExited(2500, 1)}, 2501);
        CHECK(session.state() == SessionState::Returned);
        CHECK(session.outcome().exitCode == 1u);
    }
}

TEST_CASE("launcher/launch_session: crashes and loader errors end every strategy", "[launcher][launch_session]") {
    for (const WaitStrategy strategy :
         {WaitStrategy::DirectProcess, WaitStrategy::ProcessTree, WaitStrategy::UserConfirmReturn}) {
        INFO(rv::launcher::toString(strategy));
        Fixture f;
        std::vector<ProcessEvent> events{mainExited(9000, 0xC0000005u)};
        if (strategy == WaitStrategy::ProcessTree) {
            events.push_back(event(ProcessEventKind::TreeEmpty, 9000));
        }
        const LaunchSession crashed = f.run(f.request(strategy), events, 9002);
        CHECK(crashed.state() == SessionState::Crashed);
        CHECK(crashed.outcome().exitCode == 0xC0000005u);
        CHECK(crashed.outcome().message.find("STATUS_ACCESS_VIOLATION") != std::string::npos);

        events.front().exitCode = 0xC0000135u; // a missing DLL
        const LaunchSession unloadable = f.run(f.request(strategy), events, 9002);
        CHECK(unloadable.state() == SessionState::FailedToStart);
        CHECK(unloadable.outcome().failure == FailureReason::LoaderError);
    }
}

TEST_CASE("launcher/launch_session: ProcessTree ends with the last process of the tree", "[launcher][launch_session]") {
    Fixture f;
    LaunchSession session(f.request(WaitStrategy::ProcessTree), f.runner);
    session.start(kT0);
    REQUIRE(session.strategy() == WaitStrategy::ProcessTree);

    ProcessEvent child = event(ProcessEventKind::ChildStarted, 100, kChildId);
    child.image = "emulator-core.exe";
    f.runner.queue = {child, mainExited(150, 0)};
    session.poll(at(200));
    CHECK(session.state() == SessionState::Running); // the wrapper left, its child plays on
    CHECK(session.liveProcessCount() == 1);
    REQUIRE(session.liveChildren().size() == 1);
    CHECK(session.liveChildren().front().processId == kChildId);
    CHECK(session.liveChildren().front().image == "emulator-core.exe");
    CHECK(session.liveChildren().front().seenAt == at(101));
    CHECK(session.times().mainExited == at(150));

    f.runner.queue = {event(ProcessEventKind::ChildExited, 9000, kChildId, 3u),
                      event(ProcessEventKind::TreeEmpty, 9000)};
    session.poll(at(9002));
    REQUIRE(session.state() == SessionState::Returned);
    CHECK(session.outcome().exitCode == 3u);
    CHECK(session.outcome().exitProcessId == kChildId);
    CHECK(session.outcome().processesSeen == 2);
    CHECK_FALSE(session.outcome().quickExit); // the tree ran 9 s, whatever the wrapper did
    CHECK(session.liveChildren().empty());
    CHECK(session.times().exited == at(9000));
    CHECK(session.times().ended == at(9001));
}

TEST_CASE("launcher/launch_session: a ProcessTree that ends within the startup grace", "[launcher][launch_session]") {
    Fixture f;
    SECTION("cleanly: returned, no quick exit and no hint at a launcher, since the whole tree was waited for") {
        const LaunchSession session = f.run(f.request(WaitStrategy::ProcessTree),
                                            {event(ProcessEventKind::ChildStarted, 50, kChildId), mainExited(60, 0),
                                             event(ProcessEventKind::ChildExited, 1200, kChildId, 0u),
                                             event(ProcessEventKind::TreeEmpty, 1200)},
                                            1300);
        CHECK(session.state() == SessionState::Returned);
        CHECK(session.outcome().exitProcessId == kChildId);
        CHECK_FALSE(session.outcome().quickExit);
        CHECK(session.outcome().message.find("launcher") == std::string::npos);
    }
    SECTION("uncleanly: a failure to start") {
        const LaunchSession session = f.run(f.request(WaitStrategy::ProcessTree),
                                            {event(ProcessEventKind::ChildStarted, 50, kChildId), mainExited(60, 0),
                                             event(ProcessEventKind::ChildExited, 1200, kChildId, 1u),
                                             event(ProcessEventKind::TreeEmpty, 1200)},
                                            1300);
        CHECK(session.state() == SessionState::FailedToStart);
        CHECK(session.outcome().failure == FailureReason::EarlyExit);
        CHECK(session.outcome().exitCode == 1u);
    }
}

TEST_CASE("launcher/launch_session: times count from the moment the process runs, after the hand-over",
          "[launcher][launch_session]") {
    Fixture f;
    f.runner.startedAt = at(300); // the window's hand-over took 300 ms before the process was resumed
    SECTION("the startup grace") {
        LaunchSession session(f.request(), f.runner);
        session.start(kT0);
        CHECK(session.times().begun == at(0));
        CHECK(session.times().started == at(300));
        // 2.1 s after start() was called, but 1.8 s after the program started running: within the 2 s grace.
        f.runner.queue = {mainExited(2100, 1)};
        session.poll(at(2101));
        CHECK(session.state() == SessionState::FailedToStart);
        CHECK(session.outcome().failure == FailureReason::EarlyExit);
        CHECK(session.outcome().message.find("1800 ms") != std::string::npos);
    }
    SECTION("the window timeout") {
        LaunchRequest request = f.request();
        request.timeouts.windowTimeout = 1000ms;
        LaunchSession session(request, f.runner);
        session.start(kT0);
        session.poll(at(1299));
        CHECK(session.state() == SessionState::Running);
        session.poll(at(1300));
        CHECK(session.state() == SessionState::StillRunning);
    }
}

TEST_CASE("launcher/launch_session: a session stops following the processes when it ends",
          "[launcher][launch_session]") {
    Fixture f;
    SECTION("at the emulator's exit") {
        LaunchSession session(f.request(), f.runner);
        session.start(kT0);
        f.runner.queue = {event(ProcessEventKind::ChildStarted, 100, kChildId)};
        session.poll(at(200));
        CHECK(f.runner.stops == 0);
        f.runner.queue = {mainExited(5000, 0)};
        session.poll(at(5001));
        REQUIRE(session.finished());
        CHECK(f.runner.stops == 1); // the child it leaves running is no longer watched
        session.poll(at(6000));
        session.confirmReturn(at(6001));
        CHECK(f.runner.stops == 1);
    }
    SECTION("at the user's confirmation") {
        LaunchSession session(f.request(WaitStrategy::UserConfirmReturn), f.runner);
        session.start(kT0);
        session.confirmReturn(at(1000));
        REQUIRE(session.state() == SessionState::Returned);
        CHECK(f.runner.stops == 1);
    }
    SECTION("never while it waits") {
        LaunchRequest request = f.request();
        request.timeouts.windowTimeout = 1000ms;
        LaunchSession session(request, f.runner);
        session.start(kT0);
        session.poll(at(5000));
        CHECK(session.state() == SessionState::StillRunning);
        CHECK(f.runner.stops == 0);
    }
}

TEST_CASE("launcher/launch_session: a crash anywhere in the tree", "[launcher][launch_session]") {
    Fixture f;
    const LaunchSession session = f.run(f.request(WaitStrategy::ProcessTree),
                                        {event(ProcessEventKind::ChildStarted, 100, kChildId),
                                         event(ProcessEventKind::ChildExited, 5000, kChildId, 0xC0000409u),
                                         mainExited(5200, 0), event(ProcessEventKind::TreeEmpty, 5200)},
                                        5300);
    CHECK(session.state() == SessionState::Crashed);
    CHECK(session.outcome().exitCode == 0xC0000409u);
    CHECK(session.outcome().exitProcessId == kChildId);
}

TEST_CASE("launcher/launch_session: the tree's end may be reported before the started process's exit",
          "[launcher][launch_session]") {
    Fixture f;
    const LaunchSession session =
        f.run(f.request(WaitStrategy::ProcessTree), {event(ProcessEventKind::TreeEmpty, 5000), mainExited(5000, 0)},
              5100);
    CHECK(session.state() == SessionState::Returned);
    CHECK(session.outcome().exitCode == 0u);
}

TEST_CASE("launcher/launch_session: without its tree followed, ProcessTree waits for a confirmation",
          "[launcher][launch_session]") {
    Fixture f;
    f.runner.followTree = false;
    LaunchSession session(f.request(WaitStrategy::ProcessTree), f.runner);
    session.start(kT0);
    CHECK(session.strategy() == WaitStrategy::UserConfirmReturn);
    f.runner.queue = {mainExited(5000, 0)};
    session.poll(at(5001));
    REQUIRE(session.state() == SessionState::AwaitingConfirmation);
    CHECK(session.times().exited == at(5000));
    session.confirmReturn(at(8000));
    REQUIRE(session.state() == SessionState::Returned);
    CHECK(session.outcome().confirmedByUser);
    CHECK(session.outcome().exitCode == 0u);
    CHECK(session.times().ended == at(8000));
}

TEST_CASE("launcher/launch_session: UserConfirmReturn", "[launcher][launch_session]") {
    Fixture f;
    SECTION("a launcher stub that exits at once waits for the user") {
        LaunchSession session(f.request(WaitStrategy::UserConfirmReturn), f.runner);
        session.start(kT0);
        f.runner.queue = {mainExited(120, 0)};
        session.poll(at(121));
        REQUIRE(session.state() == SessionState::AwaitingConfirmation);
        CHECK(session.outcome().quickExit);
        session.poll(at(600000)); // no timeout ends the wait
        CHECK(session.state() == SessionState::AwaitingConfirmation);
        session.confirmReturn(at(600001));
        CHECK(session.state() == SessionState::Returned);
    }
    SECTION("a confirmation while the emulator runs leaves it running") {
        LaunchSession session(f.request(WaitStrategy::UserConfirmReturn), f.runner);
        session.start(kT0);
        session.confirmReturn(at(1000));
        REQUIRE(session.state() == SessionState::Returned);
        CHECK(session.outcome().confirmedByUser);
        CHECK(session.outcome().processesRemained);
        CHECK_FALSE(session.outcome().exitCode.has_value());
    }
}

TEST_CASE("launcher/launch_session: no window for the window timeout is StillRunning, never an end",
          "[launcher][launch_session]") {
    Fixture f;
    LaunchRequest request = f.request();
    request.timeouts.windowTimeout = 1000ms;
    LaunchSession session(request, f.runner);
    session.start(kT0);
    session.poll(at(999));
    CHECK(session.state() == SessionState::Running);
    session.poll(at(1000));
    REQUIRE(session.state() == SessionState::StillRunning);
    CHECK(session.times().stillRunningSince == at(1000));

    f.runner.queue = {event(ProcessEventKind::WindowShown, 1500)};
    session.poll(at(1502));
    CHECK(session.state() == SessionState::Running); // it was only slow
    CHECK(session.times().firstWindow == at(1501));

    f.runner.queue = {event(ProcessEventKind::WindowsGone, 2000)};
    session.poll(at(2002));
    session.poll(at(3000));
    CHECK(session.state() == SessionState::Running); // 999 ms without a window
    session.poll(at(3001));
    CHECK(session.state() == SessionState::StillRunning);

    f.runner.queue = {mainExited(4000, 0)};
    session.poll(at(4001));
    REQUIRE(session.state() == SessionState::Returned);
    CHECK(session.outcome().wasStillRunning);
}

TEST_CASE("launcher/launch_session: a visible window keeps the session running", "[launcher][launch_session]") {
    Fixture f;
    LaunchRequest request = f.request();
    request.timeouts.windowTimeout = 1000ms;
    const LaunchSession session = f.run(request, {event(ProcessEventKind::WindowShown, 200)}, 3600000);
    CHECK(session.state() == SessionState::Running);
    CHECK_FALSE(session.outcome().wasStillRunning);
}

TEST_CASE("launcher/launch_session: failures before the process runs", "[launcher][launch_session]") {
    Fixture f;
    SECTION("an invalid request starts nothing") {
        LaunchRequest request = f.request();
        request.executable = f.dir.file("missing.exe");
        LaunchSession session(request, f.runner);
        session.start(kT0);
        CHECK(session.state() == SessionState::FailedToStart);
        CHECK(session.outcome().failure == FailureReason::InvalidRequest);
        CHECK(f.runner.starts == 0);
        CHECK(session.process() == nullptr);
        CHECK(session.times().ended == at(0));
    }
    SECTION("a process that cannot be created") {
        f.runner.failure = rv::Error{rv::ErrorCode::PlatformError, "CreateProcessW failed: access denied", "x"};
        LaunchSession session(f.request(), f.runner);
        session.start(kT0);
        CHECK(session.state() == SessionState::FailedToStart);
        CHECK(session.outcome().failure == FailureReason::StartFailed);
        CHECK(session.outcome().message.find("access denied") != std::string::npos);
    }
    SECTION("a runner that throws: no exception leaves the session, which is not left Starting") {
        f.runner.throwOnStart = true;
        LaunchSession session(f.request(), f.runner);
        session.start(kT0);
        CHECK(session.state() == SessionState::FailedToStart);
        CHECK(session.outcome().failure == FailureReason::StartFailed);
        CHECK(session.outcome().message.find("broke its promise") != std::string::npos);
        CHECK(session.process() == nullptr);
    }
}

TEST_CASE("launcher/launch_session: a lost watch falls back to the user's confirmation", "[launcher][launch_session]") {
    Fixture f;
    ProcessEvent lost = event(ProcessEventKind::WatchFailed, 500);
    lost.message = "the job's notifications failed";
    const LaunchSession session = f.run(f.request(), {lost, mainExited(5000, 0)}, 5001);
    CHECK(session.strategy() == WaitStrategy::UserConfirmReturn);
    CHECK(session.state() == SessionState::AwaitingConfirmation);
}

TEST_CASE("launcher/launch_session: DirectProcess records the processes it leaves running",
          "[launcher][launch_session]") {
    Fixture f;
    const LaunchSession session =
        f.run(f.request(), {event(ProcessEventKind::ChildStarted, 100, kChildId), mainExited(5000, 0)}, 5001);
    CHECK(session.state() == SessionState::Returned);
    CHECK(session.outcome().processesRemained);
    CHECK(session.liveProcessCount() == 1);
    CHECK(session.outcome().message.find("still run") != std::string::npos);
}

TEST_CASE("launcher/launch_session: the event history is bounded", "[launcher][launch_session]") {
    Fixture f;
    LaunchSession session(f.request(), f.runner);
    session.start(kT0);
    for (int i = 0; i < 400; ++i) {
        f.runner.queue.push_back(
            event(i % 2 == 0 ? ProcessEventKind::WindowShown : ProcessEventKind::WindowsGone, i));
    }
    session.poll(at(401));
    CHECK(session.history().size() == LaunchSession::kMaxHistory);
    CHECK(session.state() == SessionState::Running);
}
