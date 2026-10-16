#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/launcher/exit_status.hpp>
#include <rv/launcher/launch_request.hpp>
#include <rv/launcher/launch_session.hpp>
#include <rv/platform/win32_process_runner.hpp>

// The Win32 process runner against real processes: rv_fake_emulator.exe (spikes/launch), started from the directory of
// this executable in its windowless modes and without a console window (hideConsoleWindow), so nothing appears on
// screen. Every process a test starts is gone when the test ends; the runner itself never terminates one, so the tests
// that leave one running end it themselves (the test harness, not the product), whatever way the test ends.

using namespace std::chrono_literals;
using rv::launcher::ExitKind;
using rv::launcher::FailureReason;
using rv::launcher::LaunchClock;
using rv::launcher::LaunchRequest;
using rv::launcher::LaunchSession;
using rv::launcher::ProcessEventKind;
using rv::launcher::SessionState;
using rv::launcher::WaitStrategy;

namespace {

/// Generous for a loaded machine: a session normally ends in milliseconds.
constexpr auto kSessionLimit = 60s;

std::filesystem::path fakeEmulator() {
    return rv::core::executablePath().parent_path() / "rv_fake_emulator.exe";
}

/// A unique directory under the system's temporary directory, removed with its content.
class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        const std::string name = "rv-runner-" + std::to_string(GetCurrentProcessId()) + "-" +
                                 std::to_string(GetTickCount64()) + "-" + std::to_string(counter.fetch_add(1));
        m_path = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(m_path);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

/// The runner's wake callback, waited for by the test.
class WakeSignal {
public:
    [[nodiscard]] std::function<void()> callback() {
        return [this] {
            {
                const std::lock_guard lock(m_mutex);
                m_signaled = true;
            }
            m_condition.notify_all();
        };
    }
    void wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock(m_mutex);
        m_condition.wait_for(lock, timeout, [&] { return m_signaled; });
        m_signaled = false;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_signaled = false;
};

/// Polls the session whenever the runner wakes the test (or every 20 ms, for the timeouts) until `done` or the limit.
bool driveUntil(LaunchSession& session, WakeSignal& wake, const std::function<bool()>& done,
                std::chrono::milliseconds limit = kSessionLimit) {
    const auto deadline = LaunchClock::now() + limit;
    session.poll(LaunchClock::now());
    while (!done()) {
        if (LaunchClock::now() >= deadline) {
            return false;
        }
        wake.wait(20ms);
        session.poll(LaunchClock::now());
    }
    return true;
}

LaunchRequest fakeRequest(std::vector<std::string> arguments, WaitStrategy strategy = WaitStrategy::DirectProcess) {
    LaunchRequest request;
    request.executable = fakeEmulator();
    request.arguments = std::move(arguments);
    request.waitStrategy = strategy;
    request.label = "fake emulator";
    request.hideConsoleWindow = true; // a console program: its own console, without a window
    return request;
}

/// The arguments of a fake that hangs: never exits by itself within a test, and gives up after two minutes should
/// the test binary be killed before its clean-up.
std::vector<std::string> hangArguments() {
    return {"--hang", "--hang-limit-ms", "120000"};
}

/// An owned process handle.
class ProcessHandle {
public:
    explicit ProcessHandle(HANDLE handle = nullptr) noexcept : m_handle(handle) {}
    ProcessHandle(const ProcessHandle&) = delete;
    ProcessHandle& operator=(const ProcessHandle&) = delete;
    ~ProcessHandle() { reset(); }
    [[nodiscard]] HANDLE get() const noexcept { return m_handle; }
    void reset(HANDLE handle = nullptr) noexcept {
        if (m_handle != nullptr) {
            CloseHandle(m_handle);
        }
        m_handle = handle;
    }

private:
    HANDLE m_handle;
};

/// The harness's clean-up of a process the product leaves running on purpose: ends it when the guard goes, however the
/// test ends (a failed REQUIRE included). It holds a handle opened while the runner still held one, so the id it was
/// given can never name another process meanwhile.
class EndAtScopeExit {
public:
    explicit EndAtScopeExit(std::uint32_t processId)
        : m_process(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                processId)) {}
    EndAtScopeExit(const EndAtScopeExit&) = delete;
    EndAtScopeExit& operator=(const EndAtScopeExit&) = delete;
    ~EndAtScopeExit() { end(); }

    [[nodiscard]] bool valid() const noexcept { return m_process.get() != nullptr; }
    [[nodiscard]] bool running() const noexcept {
        return valid() && WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT;
    }
    /// Ends the process (exit code 0xDEAD) and waits for it; true when it is gone.
    bool end() noexcept {
        if (!valid()) {
            return true;
        }
        TerminateProcess(m_process.get(), 0xDEAD);
        const bool gone = WaitForSingleObject(m_process.get(), 30000) == WAIT_OBJECT_0;
        m_process.reset();
        return gone;
    }

private:
    ProcessHandle m_process;
};

nlohmann::json readJson(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    return nlohmann::json::parse(file);
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool isRunning(std::uint32_t processId) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        return false;
    }
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
}

void waitForExit(std::uint32_t processId) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process != nullptr) {
        WaitForSingleObject(process, 30000);
        CloseHandle(process);
    }
}

#define REQUIRE_FAKE_EMULATOR()                                                                                        \
    do {                                                                                                               \
        if (!std::filesystem::exists(fakeEmulator())) {                                                                \
            SKIP("rv_fake_emulator.exe is not next to the tests (RV_BUILD_SPIKES off?)");                              \
        }                                                                                                              \
    } while (false)

} // namespace

TEST_CASE("platform/process_runner: exit codes reach the session", "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    for (const std::uint32_t code : {0u, 7u, 0xFFFFFFFFu}) {
        INFO("exit code " << code);
        rv::platform::Win32ProcessRunner runner;
        WakeSignal wake;
        LaunchRequest request = fakeRequest({"--exit-code", std::to_string(code)});
        request.timeouts.startupGrace = 0ms; // classify the exit itself, however quick
        LaunchSession session(request, runner);
        session.start(LaunchClock::now(), {wake.callback()});
        REQUIRE(session.state() == SessionState::Running);
        CHECK(runner.lastStart().processId == session.processId());
        CHECK(runner.lastStart().jobAssigned);
        REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
        CHECK(session.state() == SessionState::Returned);
        CHECK(session.outcome().exitCode == code);
        CHECK(session.outcome().exitKind == (code == 0 ? ExitKind::Success : ExitKind::ErrorCode));
        REQUIRE(session.times().exited.has_value());
        REQUIRE(session.times().exitObserved.has_value());
        // The system's exit time comes before the moment it was noticed.
        CHECK(*session.times().exited <= *session.times().exitObserved);
        CHECK(*session.times().exited >= *session.times().started - 50ms);
    }
}

TEST_CASE("platform/process_runner: an unclean exit right after the start is a failure to start",
          "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    LaunchRequest request = fakeRequest({"--exit-code", "1"});
    request.timeouts.startupGrace = 10s;
    LaunchSession session(request, runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    CHECK(session.state() == SessionState::FailedToStart);
    CHECK(session.outcome().failure == FailureReason::EarlyExit);
    CHECK(session.outcome().exitCode == 1u);
}

TEST_CASE("platform/process_runner: a crash is reported as such", "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    LaunchSession session(fakeRequest({"--crash"}), runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    CHECK(session.state() == SessionState::Crashed);
    CHECK(session.outcome().exitCode == 0xC0000005u);
    CHECK(session.outcome().exitKind == ExitKind::Crash);
    CHECK(session.outcome().message.find("STATUS_ACCESS_VIOLATION") != std::string::npos);
}

TEST_CASE("platform/process_runner: ProcessTree waits for the process a wrapper started",
          "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    // The wrapper starts a copy of itself that runs 600 ms and exits with 3, and exits at once with 0.
    LaunchRequest request =
        fakeRequest({"--spawn-child-and-exit", "--sleep-ms", "600", "--exit-code", "3"}, WaitStrategy::ProcessTree);
    request.timeouts.startupGrace = 0ms;
    LaunchSession session(request, runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(session.strategy() == WaitStrategy::ProcessTree);
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));

    CHECK(session.state() == SessionState::Returned);
    CHECK(session.outcome().exitCode == 3u); // the last process of the tree, not the wrapper
    CHECK(session.outcome().exitProcessId != session.processId());
    CHECK(session.outcome().processesSeen == 2);
    CHECK_FALSE(session.outcome().processesRemained);
    REQUIRE(session.times().mainExited.has_value());
    REQUIRE(session.times().exited.has_value());
    // The child slept 600 ms after the wrapper left: a lower bound, never flaky under load.
    CHECK(*session.times().exited - *session.times().mainExited >= 400ms);

    std::vector<ProcessEventKind> kinds;
    for (const auto& event : session.history()) {
        kinds.push_back(event.kind);
    }
    REQUIRE(kinds.size() >= 4);
    CHECK(kinds.front() == ProcessEventKind::ChildStarted); // reported before the wrapper's exit
    CHECK(kinds.back() == ProcessEventKind::TreeEmpty);
    const auto childStarted = std::find_if(session.history().begin(), session.history().end(), [](const auto& event) {
        return event.kind == ProcessEventKind::ChildStarted;
    });
    REQUIRE(childStarted != session.history().end());
    CHECK(childStarted->image == "rv_fake_emulator.exe");
}

TEST_CASE("platform/process_runner: DirectProcess returns with the wrapper and reports what it left running",
          "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    LaunchSession session(fakeRequest({"--spawn-child-and-exit", "--sleep-ms", "1500"}), runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    CHECK(session.state() == SessionState::Returned);
    CHECK(session.outcome().exitCode == 0u);
    CHECK(session.outcome().quickExit); // a clean exit within the startup grace: a wrapper
    CHECK(session.outcome().processesRemained);
    CHECK(session.outcome().message.find("launcher") != std::string::npos);

    std::optional<std::uint32_t> child;
    for (const auto& event : session.history()) {
        if (event.kind == ProcessEventKind::ChildStarted) {
            child = event.processId;
        }
    }
    REQUIRE(child.has_value());
    CHECK(isRunning(*child)); // the session ended; the emulator it did not wait for runs on
    waitForExit(*child);
}

TEST_CASE("platform/process_runner: a hang becomes StillRunning and is never terminated",
          "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    std::optional<EndAtScopeExit> cleanUp; // outlives the session: the process must survive it
    {
        rv::platform::Win32ProcessRunner runner;
        WakeSignal wake;
        LaunchRequest request = fakeRequest(hangArguments());
        request.timeouts.windowTimeout = 300ms; // a windowless hang: no window for 300 ms
        LaunchSession session(request, runner);
        session.start(LaunchClock::now(), {wake.callback()});
        REQUIRE(session.processId() != 0);
        cleanUp.emplace(session.processId()); // the runner's handle keeps the id from being reused meanwhile
        REQUIRE(cleanUp->valid());
        REQUIRE(driveUntil(session, wake, [&] { return session.state() == SessionState::StillRunning; }));
        CHECK(session.outcome().wasStillRunning);
        CHECK(session.liveProcessCount() == 1);

        // The user chooses to return: the emulator is left running.
        session.confirmReturn(LaunchClock::now());
        CHECK(session.state() == SessionState::Returned);
        CHECK(session.outcome().confirmedByUser);
        CHECK(session.outcome().processesRemained);
        CHECK_FALSE(session.outcome().exitCode.has_value());
    }
    // The session, its runner and their job handles are gone: the process still runs.
    CHECK(cleanUp->running());
    CHECK(cleanUp->end());
}

TEST_CASE("platform/process_runner: the harness's termination of a hang is observed", "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    LaunchRequest request = fakeRequest(hangArguments());
    request.timeouts.windowTimeout = 0ms;
    request.timeouts.startupGrace = 0ms;
    LaunchSession session(request, runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(session.processId() != 0);
    EndAtScopeExit cleanUp(session.processId());
    REQUIRE(session.state() == SessionState::Running);
    CHECK(cleanUp.end());
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    CHECK(session.state() == SessionState::Returned);
    CHECK(session.outcome().exitCode == 0xDEADu);
}

TEST_CASE("platform/process_runner: arguments reach the program verbatim", "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    const std::vector<std::string> payload{
        "",
        " ",
        "plain",
        "two words",
        "tab\there",
        "new\nline",
        "quote\"inside",
        "\"",
        "\"\"",
        "trailing backslash\\",
        "C:\\Program Files\\Emulator\\",
        "\\\\server\\share\\rom.zip",
        "back\\\\slashes\\\\\"and a quote",
        "ends with a backslash and a quote\\\"",
        "--",
        "--looks-like-an-option",
        "-L",
        "it's an apostrophe",
        "%PATH% ^& | < > ! `$(x)`",
        "I:\\Giochi\\Emulatori\\gb\\1 GB US - H-Q\\Mole Mania (USA, Europe) (SGB Enhanced).gb",
        "Pok\xC3\xA9mon \xE2\x80\x93 \xC3\x89" "dition Bleue",
        "\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3",
        "\xF0\x9F\x8E\xAE game pad",
    };
    ScratchDir scratch;
    const std::filesystem::path output = scratch.path() / "args.json";
    std::vector<std::string> arguments{"--print-args", rv::core::pathToUtf8(output), "--"};
    arguments.insert(arguments.end(), payload.begin(), payload.end());

    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    LaunchSession session(fakeRequest(arguments), runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    REQUIRE(session.state() == SessionState::Returned);

    const nlohmann::json echoed = readJson(output);
    const auto received = echoed.at("payload").get<std::vector<std::string>>();
    REQUIRE(received.size() == payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) {
        INFO("argument " << i << ": [" << payload[i] << "]");
        CHECK(received[i] == payload[i]);
    }
    // The program saw exactly the command line the session built.
    CHECK(echoed.at("commandLine").get<std::string>() == session.commandLine());
}

TEST_CASE("platform/process_runner: environment overrides", "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    REQUIRE(SetEnvironmentVariableW(L"RV_FAKE_INHERITED", L"inherited"));
    REQUIRE(SetEnvironmentVariableW(L"RV_FAKE_REMOVED", L"removed"));
    REQUIRE(SetEnvironmentVariableW(L"RV_FAKE_KEPT", L"kept"));
    ScratchDir scratch;
    const std::filesystem::path output = scratch.path() / "env.json";

    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    LaunchRequest request = fakeRequest({"--print-env", rv::core::pathToUtf8(output)});
    request.environment = {
        {"RV_FAKE_ADDED", "\xC3\xA0\xC3\xA9 \xF0\x9F\x98\x80"}, // "àé 😀"
        {"RV_FAKE_REMOVED", std::nullopt},
        {"rv_fake_inherited", "replaced"}, // names are case-insensitive
        {"RV_FAKE_EMPTY", ""},
    };
    LaunchSession session(request, runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    SetEnvironmentVariableW(L"RV_FAKE_INHERITED", nullptr);
    SetEnvironmentVariableW(L"RV_FAKE_REMOVED", nullptr);
    SetEnvironmentVariableW(L"RV_FAKE_KEPT", nullptr);
    REQUIRE(session.state() == SessionState::Returned);

    const nlohmann::json variables = readJson(output);
    CHECK(variables.value("RV_FAKE_ADDED", "") == "\xC3\xA0\xC3\xA9 \xF0\x9F\x98\x80");
    CHECK(variables.value("RV_FAKE_INHERITED", "") == "replaced"); // the existing spelling kept
    CHECK(variables.value("RV_FAKE_KEPT", "") == "kept");
    CHECK(variables.contains("RV_FAKE_EMPTY"));
    CHECK_FALSE(variables.contains("RV_FAKE_REMOVED"));
    CHECK_FALSE(variables.contains("rv_fake_inherited"));
}

TEST_CASE("platform/process_runner: working directory and captured output", "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    ScratchDir scratch;
    const std::filesystem::path workDir = scratch.path() / "work dir \xC3\xA8";
    std::filesystem::create_directories(workDir);
    const std::filesystem::path cwdFile = scratch.path() / "cwd.json";
    const std::filesystem::path logFile = scratch.path() / "logs" / "launch.log";

    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    LaunchRequest request =
        fakeRequest({"--print-cwd", rv::core::pathToUtf8(cwdFile), "--stdout", "captured \xC3\xA8 text"});
    request.workingDirectory = workDir;
    request.outputLog = logFile;
    LaunchSession session(request, runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    REQUIRE(session.state() == SessionState::Returned);

    std::error_code ec;
    CHECK(std::filesystem::equivalent(rv::core::pathFromUtf8(readJson(cwdFile).at("cwd").get<std::string>()), workDir,
                                      ec));
    const std::string log = readText(logFile);
    CHECK(log.starts_with("--- ")); // the runner's header, with the command line
    std::istringstream lines(log);
    std::size_t printed = 0;
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        printed += line == "captured \xC3\xA8 text" ? 1u : 0u;
    }
    CHECK(printed == 2); // standard output and standard error
}

TEST_CASE("platform/process_runner: files that are not programs are refused before anything starts",
          "[platform][process_runner]") {
    ScratchDir scratch;
    const std::filesystem::path text = scratch.path() / "not a program.exe";
    std::ofstream(text) << "this is text, not a Windows program\n";
    const std::filesystem::path dos = scratch.path() / "dos.com";
    std::ofstream(dos, std::ios::binary) << "MZ" << std::string(80, ' '); // a DOS header without a PE signature
    rv::platform::Win32ProcessRunner runner;
    for (const std::filesystem::path& executable : {text, dos}) {
        LaunchRequest request;
        request.executable = executable;
        LaunchSession session(request, runner);
        session.start(LaunchClock::now());
        CHECK(session.state() == SessionState::FailedToStart);
        CHECK(session.outcome().failure == FailureReason::InvalidRequest);
        CHECK(session.process() == nullptr);
    }
}

TEST_CASE("platform/process_runner: an image Windows cannot load fails to start, without a dialog",
          "[platform][process_runner]") {
    ScratchDir scratch;
    // A PE signature over garbage: validation reads the headers only, CreateProcessW refuses the image.
    const std::filesystem::path broken = scratch.path() / "broken program.exe";
    std::string bytes(512, 'Z');
    bytes[0] = 'M';
    bytes[1] = 'Z';
    bytes[0x3C] = 0x40;
    bytes[0x3D] = 0;
    bytes[0x3E] = 0;
    bytes[0x3F] = 0;
    const char signature[] = {'P', 'E', '\0', '\0', '\x64', '\x86'}; // x64
    bytes.replace(0x40, sizeof(signature), signature, sizeof(signature));
    std::ofstream(broken, std::ios::binary) << bytes;
    rv::platform::Win32ProcessRunner runner;
    LaunchRequest request;
    request.executable = broken;
    LaunchSession session(request, runner);
    session.start(LaunchClock::now());
    CHECK(session.state() == SessionState::FailedToStart);
    CHECK(session.outcome().failure == FailureReason::StartFailed);
    CHECK(session.outcome().message.find("CreateProcessW") != std::string::npos);
    CHECK(session.process() == nullptr);

    // The runner on its own, given a text file (validation skipped): Windows would show a modal "unsupported 16-bit
    // application" box from CreateProcessW; the runner's thread error mode makes the call fail instead.
    const std::filesystem::path text = scratch.path() / "text.exe";
    std::ofstream(text) << "not a program\n";
    LaunchRequest unchecked;
    unchecked.executable = text;
    const auto started = runner.start(unchecked, {});
    CHECK_FALSE(started.has_value());
}

TEST_CASE("platform/process_runner: a request is validated before anything starts", "[platform][process_runner]") {
    ScratchDir scratch;
    const std::filesystem::path batch = scratch.path() / "wrapper.bat";
    std::ofstream(batch) << "@echo off\r\n";
    rv::platform::Win32ProcessRunner runner;
    for (const std::filesystem::path& executable : {batch, scratch.path() / "missing.exe"}) {
        LaunchRequest request;
        request.executable = executable;
        LaunchSession session(request, runner);
        session.start(LaunchClock::now());
        CHECK(session.state() == SessionState::FailedToStart);
        CHECK(session.outcome().failure == FailureReason::InvalidRequest);
        CHECK(session.process() == nullptr);
    }
}

TEST_CASE("platform/process_runner: a child that breaks away from the job is not waited for",
          "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    ScratchDir scratch;
    const std::filesystem::path logFile = scratch.path() / "wrapper.log";
    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    // JOB_OBJECT_LIMIT_BREAKAWAY_OK lets a child that asks for it leave the job: the tree ends with the wrapper.
    LaunchRequest request = fakeRequest({"--spawn-child-and-exit", "--child-breakaway", "--sleep-ms", "1500"},
                                        WaitStrategy::ProcessTree);
    request.outputLog = logFile;
    LaunchSession session(request, runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    if (session.outcome().exitCode == 3u) {
        SKIP("a job this test runs in forbids breaking away (the wrapper could not start its child)");
    }
    CHECK(session.state() == SessionState::Returned);
    CHECK(session.outcome().processesSeen == 1);

    // The wrapper printed the child's id: it runs on outside the tree.
    const std::string log = readText(logFile);
    const std::size_t at = log.find("started process ");
    REQUIRE(at != std::string::npos);
    const auto child = static_cast<std::uint32_t>(std::stoul(log.substr(at + 16)));
    CHECK(isRunning(child));
    waitForExit(child);
}

TEST_CASE("platform/process_runner: an exception before the program runs ends it and is reported as an error",
          "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    ScratchDir scratch;
    const std::filesystem::path output = scratch.path() / "args.json";
    const LaunchRequest request = fakeRequest({"--print-args", rv::core::pathToUtf8(output), "--", "payload"});

    SECTION("the runner on its own") {
        rv::platform::Win32ProcessRunner runner;
        ProcessHandle observed;
        rv::launcher::StartCallbacks callbacks;
        callbacks.beforeResume = [&](std::uint32_t processId) {
            observed.reset(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
            throw std::runtime_error("the hand-over failed");
        };
        const auto started = runner.start(request, std::move(callbacks));
        REQUIRE_FALSE(started.has_value());
        CHECK(started.error().message.find("the hand-over failed") != std::string::npos);
        REQUIRE(observed.get() != nullptr);
        // Ended before start() returned, with the code of a cancelled operation, without having run.
        CHECK(WaitForSingleObject(observed.get(), 0) == WAIT_OBJECT_0);
        DWORD code = 0;
        CHECK(GetExitCodeProcess(observed.get(), &code));
        CHECK(code == ERROR_CANCELLED);
        CHECK_FALSE(std::filesystem::exists(output));
    }
    SECTION("through a session: a failure to start, not an exception") {
        rv::platform::Win32ProcessRunner runner;
        LaunchSession session(request, runner);
        session.start(LaunchClock::now(), {nullptr, [](std::uint32_t) { throw std::runtime_error("no window"); }});
        CHECK(session.state() == SessionState::FailedToStart);
        CHECK(session.outcome().failure == FailureReason::StartFailed);
        CHECK(session.outcome().message.find("no window") != std::string::npos);
        CHECK(session.process() == nullptr);
        CHECK_FALSE(std::filesystem::exists(output));
    }
}

TEST_CASE("platform/process_runner: the start time is the moment the program runs, after the hand-over",
          "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    LaunchSession session(fakeRequest({}), runner);
    const LaunchClock::time_point begin = LaunchClock::now();
    session.start(begin, {wake.callback(), [](std::uint32_t) { std::this_thread::sleep_for(200ms); }});
    REQUIRE(session.state() == SessionState::Running);
    CHECK(session.times().begun == begin);
    REQUIRE(session.times().started.has_value());
    CHECK(*session.times().started >= begin + 200ms); // the hand-over took 200 ms
    CHECK(session.process()->startedAt() == *session.times().started);
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    CHECK(session.state() == SessionState::Returned);
    CHECK(*session.times().exited >= *session.times().started - 50ms);
}

TEST_CASE("platform/process_runner: a finished session stops following what it leaves running",
          "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    rv::platform::Win32ProcessRunner runner;
    std::atomic<int> wakes{0};
    WakeSignal wake;
    const auto signal = wake.callback();
    // DirectProcess with a wrapper: the session ends at once; the wrapper's child runs on for a second.
    LaunchSession session(fakeRequest({"--spawn-child-and-exit", "--sleep-ms", "1000"}), runner);
    session.start(LaunchClock::now(), {[&] {
                                           ++wakes;
                                           signal();
                                       }});
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    REQUIRE(session.state() == SessionState::Returned);
    REQUIRE(session.liveChildren().size() == 1);
    const std::uint32_t child = session.liveChildren().front().processId;
    CHECK(session.liveChildren().front().image == "rv_fake_emulator.exe");
    const int wakesAtEnd = wakes.load();

    // The child's exit empties the tree: a runner still following it would queue both events and wake the caller for
    // each (it queues nothing without waking).
    waitForExit(child);
    wake.wait(1000ms);
    CHECK(wakes.load() == wakesAtEnd);
}

TEST_CASE("platform/process_runner: a crash after hundreds of processes in the tree is still classified",
          "[platform][process_runner]") {
    REQUIRE_FAKE_EMULATOR();
    rv::platform::Win32ProcessRunner runner;
    WakeSignal wake;
    // The wrapper runs 300 short-lived copies of itself, then starts the one that crashes after 500 ms, and exits.
    // Only running processes are tracked (an id given to a new process names the new one), so no count of processes
    // before it keeps the crash from being seen.
    LaunchRequest request = fakeRequest(
        {"--spawn-children", "300", "--spawn-child-and-exit", "--sleep-ms", "500", "--crash"},
        WaitStrategy::ProcessTree);
    request.timeouts.startupGrace = 0ms;
    LaunchSession session(request, runner);
    session.start(LaunchClock::now(), {wake.callback()});
    REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
    CHECK(session.state() == SessionState::Crashed);
    CHECK(session.outcome().exitCode == 0xC0000005u);
    CHECK(session.outcome().exitProcessId != session.processId());
    CHECK(session.outcome().processesSeen > 256);
}

TEST_CASE("platform/process_runner: a console program never shares Retroverse's console", "[platform][process_runner]") {
    SECTION("its own console, windowless on request; GUI programs ignore both flags") {
        LaunchRequest visible;
        const std::uint32_t shown = rv::platform::processCreationFlags(visible);
        CHECK((shown & CREATE_NEW_CONSOLE) != 0);
        CHECK((shown & CREATE_NO_WINDOW) == 0);
        LaunchRequest hidden;
        hidden.hideConsoleWindow = true;
        const std::uint32_t windowless = rv::platform::processCreationFlags(hidden);
        CHECK((windowless & CREATE_NO_WINDOW) != 0);
        CHECK((windowless & CREATE_NEW_CONSOLE) == 0);
        for (const std::uint32_t flags : {shown, windowless}) {
            CHECK((flags & CREATE_SUSPENDED) != 0);
            CHECK((flags & CREATE_DEFAULT_ERROR_MODE) != 0);
            CHECK((flags & CREATE_UNICODE_ENVIRONMENT) != 0);
            CHECK((flags & DETACHED_PROCESS) == 0);
            CHECK((flags & EXTENDED_STARTUPINFO_PRESENT) == 0);
        }
        hidden.outputLog = "C:\\logs\\launch.log";
        CHECK((rv::platform::processCreationFlags(hidden) & EXTENDED_STARTUPINFO_PRESENT) != 0);
    }
    SECTION("the processes attached to the started program's console") {
        REQUIRE_FAKE_EMULATOR();
        ScratchDir scratch;
        const std::filesystem::path output = scratch.path() / "console.json";
        rv::platform::Win32ProcessRunner runner;
        WakeSignal wake;
        LaunchSession session(fakeRequest({"--print-console", rv::core::pathToUtf8(output)}), runner);
        session.start(LaunchClock::now(), {wake.callback()});
        const std::uint32_t started = session.processId();
        REQUIRE(driveUntil(session, wake, [&] { return session.finished(); }));
        const nlohmann::json console = readJson(output);
        const auto processes = console.at("processes").get<std::vector<std::uint32_t>>();
        CHECK(console.at("hasConsole").get<bool>());
        CHECK(std::find(processes.begin(), processes.end(), started) != processes.end());
        CHECK(std::find(processes.begin(), processes.end(), GetCurrentProcessId()) == processes.end());
    }
}
