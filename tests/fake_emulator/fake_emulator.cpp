// rv_fake_emulator: a stand-in for an emulator in the launch tests (tests/unit/platform_win, tests/unit/support, the
// launch spike's desktop cases; docs/spikes/i1-launch.md). It behaves like the cases the launcher must handle (a clean
// or failing exit, a crash, a hang, a wrapper that starts the real program and exits, a fullscreen window, an exclusive
// display mode, a window that grabs the foreground) and writes down what it saw: its arguments, environment and working
// directory (quoting and environment round trips), whether it got the foreground and which display mode it ran in.
//
//   rv_fake_emulator [options] [-- payload arguments...]
//
// With RV_LAUNCH_BACKGROUND set (automated test runs; docs/i2/step-focus.md) it never takes the foreground and never
// injects input: its window is an inactive tool window off every monitor, and the foreground options are skipped.
//
// Options are read up to the first "--"; everything after it is payload, echoed by --print-args and never interpreted.
// The program never terminates another process; with --exit-timeout-ms it ends itself.

#include <SDL3/SDL_main.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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

#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::uint32_t exitCode = 0;
    int frames = 0;
    bool exclusive = false;
    bool stealFocus = false;
    bool inputAtEnd = false;
    int sleepMs = 0;
    bool crash = false;
    bool hang = false;
    int hangLimitMs = 0;
    int exitTimeoutMs = 0;
    bool spawnChildAndExit = false;
    bool childBreakaway = false;
    int spawnChildren = 0;
    int leaveForegroundWindowMs = 0;
    int foregroundWindowMs = 0;
    bool lockForeground = false;
    bool hungForeground = false;
    std::string readyEvent;
    std::string printArgs;
    std::string printEnv;
    std::string printCwd;
    std::string printConsole;
    std::string stdoutText;
    std::string outText;
    std::string errText;
    std::string log;
    std::uint32_t color = 0x7A2E8C;
    std::vector<std::string> payload;
};

constexpr std::string_view kUsage =
    "rv_fake_emulator [options] [-- payload...]\n"
    "  --exit-code N            exit with N (decimal or 0x hexadecimal; default 0)\n"
    "  --frames N               show a fullscreen window for N frames (vsync), then exit\n"
    "  --exclusive              with --frames: exclusive fullscreen in a display mode other than the desktop's\n"
    "  --steal-focus            with --frames: force the window into the foreground on its first frame\n"
    "  --input-at-end           with --frames: a Shift key tap into its window before it closes (the\n"
    "                           player's last key press: the foreground lock is active when it exits)\n"
    "  --leave-foreground-window MS\n"
    "                           with --frames: before exiting, start a copy of this program that shows a small\n"
    "                           window holding the foreground for MS ms (another application in the foreground\n"
    "                           when the emulator is gone), and wait until it has the foreground\n"
    "  --lock-foreground        with --leave-foreground-window: that window also calls LockSetForegroundWindow\n"
    "  --hung-foreground        with --leave-foreground-window: that window stops responding once it has the\n"
    "                           foreground, and this program exits 5.5 s later, once Windows considers it hung\n"
    "  --foreground-window MS   (the copy) a small window that takes the foreground and keeps it for MS ms\n"
    "  --ready-event NAME       (the copy) the event it sets once its window has the foreground\n"
    "  --sleep-ms N             wait N ms (without a window) before exiting\n"
    "  --crash                  end with an access violation (0xC0000005) after the frames and the sleep; Windows'\n"
    "                           error dialogs and error reporting are turned off for it\n"
    "  --hang                   never exit (the test harness ends the process)\n"
    "  --hang-limit-ms N        with --hang: exit after N ms anyway (with --exit-code), so that a test harness that\n"
    "                           is killed before its clean-up leaves nothing running for long\n"
    "  --exit-timeout-ms N      whatever it is doing, end after N ms with exit code 124: a fake abandoned by a test\n"
    "                           harness that died ends by itself (copies started by --spawn-child-and-exit inherit\n"
    "                           it)\n"
    "  --spawn-children N       first start N copies of this program without options (each exits at once), at\n"
    "                           most 16 at a time, and wait for them\n"
    "  --spawn-child-and-exit   start a copy of this program with the other options and exit at once\n"
    "  --child-breakaway        with --spawn-child-and-exit: the copy leaves the job (CREATE_BREAKAWAY_FROM_JOB)\n"
    "  --print-args FILE        write the payload, argv and the raw command line to FILE (JSON)\n"
    "  --print-env FILE         write the RV_FAKE_* environment variables to FILE (JSON)\n"
    "  --print-cwd FILE         write the current directory to FILE (JSON)\n"
    "  --print-console FILE     write the processes attached to its console to FILE (JSON)\n"
    "  --stdout TEXT            print TEXT on standard output and standard error\n"
    "  --out TEXT               print TEXT on standard output only\n"
    "  --err TEXT               print TEXT on standard error only\n"
    "  --log FILE               write what the run saw (foreground, display modes, times) to FILE (JSON)\n"
    "  --color RRGGBB           window color\n"
    "With RV_LAUNCH_BACKGROUND set (and neither empty nor 0: every automated test run but the desktop tests), the\n"
    "program never takes the foreground and never injects input: the --frames window is a small inactive tool window\n"
    "off every monitor (no fullscreen, no exclusive mode, no raise), and --steal-focus, --input-at-end,\n"
    "--leave-foreground-window and --foreground-window are skipped (the log says so).\n";

std::optional<std::uint64_t> parseNumber(std::string_view text) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        int digit = -1;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (base == 16 && c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (base == 16 && c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        }
        if (digit < 0) {
            return std::nullopt;
        }
        value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
        if (value > 0xFFFFFFFFull) {
            return std::nullopt;
        }
    }
    return value;
}

bool parse(const std::vector<std::string>& args, Options& options, std::string& error) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--") {
            options.payload.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            return true;
        }
        const auto value = [&](std::string& out) {
            if (i + 1 >= args.size()) {
                error = "missing value for " + arg;
                return false;
            }
            out = args[++i];
            return true;
        };
        const auto number = [&](std::uint64_t& out) {
            std::string text;
            if (!value(text)) {
                return false;
            }
            const auto parsed = parseNumber(text);
            if (!parsed) {
                error = "not a number for " + arg + ": " + text;
                return false;
            }
            out = *parsed;
            return true;
        };
        std::uint64_t n = 0;
        if (arg == "--exit-code") {
            if (!number(n)) {
                return false;
            }
            options.exitCode = static_cast<std::uint32_t>(n);
        } else if (arg == "--frames") {
            if (!number(n)) {
                return false;
            }
            options.frames = static_cast<int>(std::min<std::uint64_t>(n, 1000000));
        } else if (arg == "--sleep-ms") {
            if (!number(n)) {
                return false;
            }
            options.sleepMs = static_cast<int>(std::min<std::uint64_t>(n, 600000));
        } else if (arg == "--hang-limit-ms") {
            if (!number(n)) {
                return false;
            }
            options.hangLimitMs = static_cast<int>(std::min<std::uint64_t>(n, 3600000));
        } else if (arg == "--exit-timeout-ms") {
            if (!number(n)) {
                return false;
            }
            options.exitTimeoutMs = static_cast<int>(std::min<std::uint64_t>(n, 86400000));
        } else if (arg == "--spawn-children") {
            if (!number(n)) {
                return false;
            }
            options.spawnChildren = static_cast<int>(std::min<std::uint64_t>(n, 4096));
        } else if (arg == "--leave-foreground-window") {
            if (!number(n)) {
                return false;
            }
            options.leaveForegroundWindowMs = static_cast<int>(std::min<std::uint64_t>(n, 600000));
        } else if (arg == "--foreground-window") {
            if (!number(n)) {
                return false;
            }
            options.foregroundWindowMs = static_cast<int>(std::min<std::uint64_t>(n, 600000));
        } else if (arg == "--lock-foreground") {
            options.lockForeground = true;
        } else if (arg == "--hung-foreground") {
            options.hungForeground = true;
        } else if (arg == "--ready-event") {
            if (!value(options.readyEvent)) {
                return false;
            }
        } else if (arg == "--print-console") {
            if (!value(options.printConsole)) {
                return false;
            }
        } else if (arg == "--color") {
            std::string text;
            if (!value(text)) {
                return false;
            }
            const auto parsed = parseNumber("0x" + text);
            if (!parsed) {
                error = "not a color: " + text;
                return false;
            }
            options.color = static_cast<std::uint32_t>(*parsed);
        } else if (arg == "--exclusive") {
            options.exclusive = true;
        } else if (arg == "--steal-focus") {
            options.stealFocus = true;
        } else if (arg == "--input-at-end") {
            options.inputAtEnd = true;
        } else if (arg == "--crash") {
            options.crash = true;
        } else if (arg == "--hang") {
            options.hang = true;
        } else if (arg == "--spawn-child-and-exit") {
            options.spawnChildAndExit = true;
        } else if (arg == "--child-breakaway") {
            options.childBreakaway = true;
        } else if (arg == "--print-args") {
            if (!value(options.printArgs)) {
                return false;
            }
        } else if (arg == "--print-env") {
            if (!value(options.printEnv)) {
                return false;
            }
        } else if (arg == "--print-cwd") {
            if (!value(options.printCwd)) {
                return false;
            }
        } else if (arg == "--stdout") {
            if (!value(options.stdoutText)) {
                return false;
            }
        } else if (arg == "--out") {
            if (!value(options.outText)) {
                return false;
            }
        } else if (arg == "--err") {
            if (!value(options.errText)) {
                return false;
            }
        } else if (arg == "--log") {
            if (!value(options.log)) {
                return false;
            }
        } else if (arg == "--help" || arg == "-h") {
            error = "help";
            return false;
        } else {
            error = "unknown option: " + arg;
            return false;
        }
    }
    return true;
}

/// Whether this run is in the background (RV_LAUNCH_BACKGROUND set, and neither empty nor "0", as
/// rv::platform_win::backgroundLaunchesRequested reads it): an automated test run that must leave the foreground, the
/// keyboard and the screen to the user at the machine.
bool inBackground() {
    static const bool background = [] {
        wchar_t value[8] = {};
        const DWORD length = GetEnvironmentVariableW(L"RV_LAUNCH_BACKGROUND", value, 8);
        return length != 0 && !(length == 1 && value[0] == L'0');
    }();
    return background;
}

/// The exit code of a run ended by --exit-timeout-ms (as GNU timeout's).
constexpr UINT kExitTimeoutCode = 124;

/// Ends this process after `limitMs` whatever it is doing (--exit-timeout-ms): a detached thread that never returns.
void armExitTimeout(int limitMs) {
    std::thread([limitMs] {
        std::this_thread::sleep_for(std::chrono::milliseconds(limitMs));
        std::fflush(stdout);
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), kExitTimeoutCode);
    }).detach();
}

void writeJson(const std::string& path, const nlohmann::json& json) {
    if (path.empty()) {
        return;
    }
    // Errors are reported on stderr: the tests that read the file fail on its absence.
    if (auto written = rv::core::writeTextFileAtomic(rv::core::pathFromUtf8(path), json.dump(2)); !written) {
        std::fprintf(stderr, "rv_fake_emulator: %s\n", written.error().toString().c_str());
    }
}

nlohmann::json describeMode(const SDL_DisplayMode* mode) {
    if (mode == nullptr) {
        return nullptr;
    }
    return nlohmann::json{{"width", mode->w}, {"height", mode->h}, {"refreshRate", mode->refresh_rate},
                          {"pixelDensity", mode->pixel_density}};
}

/// Starts a copy of this program with `args`; its process handle (to be closed by the caller), or nullptr.
HANDLE startCopy(const std::vector<std::string>& args, DWORD flags, DWORD* processId = nullptr) {
    const std::filesystem::path self = rv::core::executablePath();
    const auto commandLine = rv::launcher::buildCommandLine(rv::core::pathToUtf8Lossy(self), args);
    if (!commandLine) {
        std::fprintf(stderr, "rv_fake_emulator: %s\n", commandLine.error().toString().c_str());
        return nullptr;
    }
    auto wide = rv::core::utf8ToWide(*commandLine);
    if (!wide) {
        std::fprintf(stderr, "rv_fake_emulator: %s\n", wide.error().toString().c_str());
        return nullptr;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(self.c_str(), wide->data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &startup, &info)) {
        std::fprintf(stderr, "rv_fake_emulator: CreateProcessW failed with error %lu\n", GetLastError());
        return nullptr;
    }
    CloseHandle(info.hThread);
    if (processId != nullptr) {
        *processId = info.dwProcessId;
    }
    return info.hProcess;
}

/// Starts a copy of this program with `args` (its own options minus --spawn-child-and-exit) and returns at once.
bool spawnChild(const std::vector<std::string>& args, bool breakaway) {
    DWORD processId = 0;
    const HANDLE process = startCopy(args, breakaway ? CREATE_BREAKAWAY_FROM_JOB : 0, &processId);
    if (process == nullptr) {
        return false;
    }
    CloseHandle(process);
    std::printf("rv_fake_emulator: started process %lu\n", processId);
    return true;
}

/// Starts `count` copies without options (each exits at once), at most 16 at a time, and waits for them all.
bool spawnChildren(int count) {
    std::vector<HANDLE> running;
    bool ok = true;
    const auto waitForAll = [&] {
        if (!running.empty()) {
            WaitForMultipleObjects(static_cast<DWORD>(running.size()), running.data(), TRUE, INFINITE);
            for (const HANDLE process : running) {
                CloseHandle(process);
            }
            running.clear();
        }
    };
    for (int i = 0; i < count && ok; ++i) {
        const HANDLE process = startCopy({}, 0);
        ok = process != nullptr;
        if (ok) {
            running.push_back(process);
        }
        if (running.size() == 16) {
            waitForAll();
        }
    }
    waitForAll();
    return ok;
}

/// The process owning the foreground window, 0 when there is none.
DWORD foregroundProcess() {
    DWORD processId = 0;
    if (const HWND window = GetForegroundWindow(); window != nullptr) {
        GetWindowThreadProcessId(window, &processId);
    }
    return processId;
}

/// Windows considers a window hung once its thread has not read its messages for this long (IsHungAppWindow).
constexpr int kHungAfterMs = 5000;

/// Starts the copy that holds the foreground after this process exits (--leave-foreground-window) and waits, at most
/// 3 s, until its window has the foreground (its ready event); with --hung-foreground, also until Windows considers
/// that window hung. Fills `log`.
void leaveForegroundWindow(const Options& options, nlohmann::json& log) {
    const std::string eventName =
        "Local\\rv-fake-emulator-foreground-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64());
    const std::wstring wideName(eventName.begin(), eventName.end()); // ASCII
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, wideName.c_str());
    std::vector<std::string> args{"--foreground-window", std::to_string(options.leaveForegroundWindowMs),
                                  "--ready-event", eventName};
    if (options.lockForeground) {
        args.emplace_back("--lock-foreground");
    }
    if (options.hungForeground) {
        args.emplace_back("--hung-foreground");
    }
    if (!options.log.empty()) {
        args.insert(args.end(), {"--log", options.log + ".helper.json"});
    }
    DWORD processId = 0;
    const Clock::time_point start = Clock::now();
    const HANDLE helper = ready != nullptr ? startCopy(args, 0, &processId) : nullptr;
    if (helper == nullptr) {
        log["foregroundHelper"] = {{"started", false}};
        if (ready != nullptr) {
            CloseHandle(ready);
        }
        return;
    }
    const std::array<HANDLE, 2> handles{ready, helper};
    const bool isReady = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, 3000) ==
                         WAIT_OBJECT_0;
    const double readyMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    if (isReady && options.hungForeground) {
        Sleep(kHungAfterMs + 500);
    }
    log["foregroundHelper"] = {{"started", true},
                               {"processId", processId},
                               {"ready", isReady},
                               {"readyMs", readyMs},
                               {"foregroundAtExit", foregroundProcess() == processId}};
    CloseHandle(helper);
    CloseHandle(ready);
}

/// The copy started by --leave-foreground-window: a small window that takes the foreground (its parent, the
/// foreground process, started it), keeps it for `options.foregroundWindowMs` and sets the ready event once it has it
/// (and has locked it, with --lock-foreground). With --hung-foreground it then stops reading its messages until its
/// time is up. Fills `log`.
void runForegroundWindow(const Options& options, nlohmann::json& log) {
    if (inBackground()) {
        log["foregroundWindow"] = {{"skipped", "background (RV_LAUNCH_BACKGROUND)"}};
        return;
    }
    const Clock::time_point start = Clock::now();
    const auto sinceStart = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log["error"] = std::string("SDL_Init failed: ") + SDL_GetError();
        return;
    }
    SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
    SDL_Window* window = SDL_CreateWindow("Retroverse fake emulator: another application", 640, 360, 0);
    SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (renderer == nullptr) {
        log["error"] = std::string("no window: ") + SDL_GetError();
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
        SDL_Quit();
        return;
    }
    SDL_SetRenderVSync(renderer, 1);
    const auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    const std::wstring eventName(options.readyEvent.begin(), options.readyEvent.end()); // ASCII
    const HANDLE ready = eventName.empty() ? nullptr : OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());
    SDL_RaiseWindow(window);
    std::optional<double> foregroundAt;
    std::optional<double> lostAt;
    bool locked = false;
    bool hung = false;
    while (sinceStart() < options.foregroundWindowMs) {
        if (hung) {
            Sleep(10); // not reading its messages: Windows soon considers the window hung
            continue;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
        }
        const bool foreground = hwnd != nullptr && GetForegroundWindow() == hwnd;
        if (foreground && !foregroundAt) {
            foregroundAt = sinceStart();
            if (options.lockForeground) {
                locked = LockSetForegroundWindow(LSFW_LOCK) != FALSE;
            }
            if (ready != nullptr) {
                SetEvent(ready);
            }
            hung = options.hungForeground;
        }
        if (!foreground && foregroundAt && !lostAt) {
            lostAt = sinceStart();
        }
        SDL_SetRenderDrawColor(renderer, 40, 90, 160, 255);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);
    }
    log["foregroundWindow"] = {{"foregroundAtMs", foregroundAt ? nlohmann::json(*foregroundAt) : nlohmann::json()},
                               {"lostAtMs", lostAt ? nlohmann::json(*lostAt) : nlohmann::json()},
                               {"locked", locked},
                               {"hung", hung},
                               {"foregroundAtEnd", hwnd != nullptr && GetForegroundWindow() == hwnd}};
    if (locked) {
        LockSetForegroundWindow(LSFW_UNLOCK);
    }
    if (ready != nullptr) {
        CloseHandle(ready);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

[[noreturn]] void crashNow() {
    // No error report dialog: the test harness is waiting for the exit code.
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    volatile int* volatile target = nullptr;
    *target = 0x0BADF00D;
    std::abort();
}

/// Never returns, unless `limitMs` is positive: then it returns after that long.
void hang(int limitMs) {
    if (limitMs > 0) {
        Sleep(static_cast<DWORD>(limitMs));
        return;
    }
    while (true) {
        Sleep(INFINITE);
    }
}

/// The exclusive mode to use: the lowest-resolution mode of the display that differs from its desktop mode.
const SDL_DisplayMode* pickExclusiveMode(const SDL_DisplayMode* desktop, SDL_DisplayMode** modes, int count) {
    const SDL_DisplayMode* chosen = nullptr;
    for (int i = 0; i < count; ++i) {
        const SDL_DisplayMode* mode = modes[i];
        if (desktop != nullptr && mode->w == desktop->w && mode->h == desktop->h) {
            continue;
        }
        if (mode->w < 1024 || mode->h < 600) {
            continue; // modes that small may not be offered by every monitor's scaler
        }
        if (chosen == nullptr || mode->w * mode->h < chosen->w * chosen->h ||
            (mode->w * mode->h == chosen->w * chosen->h && std::abs(mode->refresh_rate - 60.0f) <
                                                              std::abs(chosen->refresh_rate - 60.0f))) {
            chosen = mode;
        }
    }
    return chosen;
}

/// The window part: a fullscreen window for `options.frames` frames. Fills `log`.
void runWindow(const Options& options, nlohmann::json& log) {
    const Clock::time_point start = Clock::now();
    const auto sinceStart = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        log["error"] = std::string("SDL_Init failed: ") + SDL_GetError();
        return;
    }
    const bool background = inBackground();
    if (options.stealFocus && !background) {
        SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
    }
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    log["display"]["desktop"] = describeMode(desktop);
    log["background"] = background;

    // In the background: a small tool window (no taskbar button) that never takes the focus, still a visible window
    // of the process for the launcher's window detection.
    const SDL_WindowFlags flags =
        SDL_WINDOW_HIDDEN | (background ? SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_UTILITY : SDL_WindowFlags{0});
    SDL_Window* window =
        SDL_CreateWindow("Retroverse fake emulator", background ? 320 : 1280, background ? 180 : 720, flags);
    if (window == nullptr) {
        log["error"] = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        SDL_Quit();
        return;
    }
    const auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (background) {
        // Off every monitor, left of the virtual screen, shown without activation at the bottom of the z-order.
        if (hwnd != nullptr) {
            const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) - 2000;
            const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
            SetWindowPos(hwnd, HWND_BOTTOM, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        log["display"]["requestedExclusive"] = nullptr;
        if (options.exclusive) {
            log["warning"] = "background (RV_LAUNCH_BACKGROUND): an inactive window off-screen, no exclusive mode";
        }
    } else if (options.exclusive) {
        int count = 0;
        SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count);
        const SDL_DisplayMode* mode = modes != nullptr ? pickExclusiveMode(desktop, modes, count) : nullptr;
        if (mode != nullptr && SDL_SetWindowFullscreenMode(window, mode)) {
            log["display"]["requestedExclusive"] = describeMode(mode);
        } else {
            log["display"]["requestedExclusive"] = nullptr;
            log["warning"] = "no exclusive mode other than the desktop's: borderless fullscreen";
        }
        SDL_free(modes);
    }
    if (!background) {
        SDL_SetWindowFullscreen(window, true);
        SDL_ShowWindow(window);
    }
    SDL_SyncWindow(window);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        log["error"] = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }
    SDL_SetRenderVSync(renderer, 1);
    log["display"]["current"] = describeMode(SDL_GetCurrentDisplayMode(display));
    log["windowShownMs"] = sinceStart();

    std::optional<double> firstForeground;
    std::vector<bool> samples;
    int frame = 0;
    bool quit = false;
    const auto r = static_cast<std::uint8_t>((options.color >> 16) & 0xFF);
    const auto g = static_cast<std::uint8_t>((options.color >> 8) & 0xFF);
    const auto b = static_cast<std::uint8_t>(options.color & 0xFF);
    while (!quit && (options.hang || frame < options.frames)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                quit = true;
            }
        }
        if (frame == 0 && options.stealFocus && !background) {
            SDL_RaiseWindow(window);
        }
        const bool foreground = hwnd != nullptr && GetForegroundWindow() == hwnd;
        if (foreground && !firstForeground) {
            firstForeground = sinceStart();
        }
        if (frame == 0 || frame % 30 == 0) {
            samples.push_back(foreground);
        }
        int width = 0;
        int height = 0;
        SDL_GetRenderOutputSize(renderer, &width, &height);
        SDL_SetRenderDrawColor(renderer, r, g, b, 255);
        SDL_RenderClear(renderer);
        // A bar sweeping across the screen: the frame pacing shows.
        const float x = static_cast<float>((frame * 16) % std::max(width, 1));
        const SDL_FRect bar{x, 0.0f, static_cast<float>(width) / 16.0f, static_cast<float>(height)};
        SDL_SetRenderDrawColor(renderer, 240, 220, 160, 255);
        SDL_RenderFillRect(renderer, &bar);
        SDL_RenderPresent(renderer);
        ++frame;
    }
    if (options.stealFocus && background) {
        log["stealFocus"] = {{"skipped", "background (RV_LAUNCH_BACKGROUND)"}};
    }
    if (options.inputAtEnd && background) {
        // Never input into whatever the user is doing.
        log["inputAtEnd"] = {{"sent", false}, {"skipped", "background (RV_LAUNCH_BACKGROUND)"}};
    } else if (options.inputAtEnd) {
        // While the window is in the foreground, the tap is its input: the last input event belongs to this
        // process, as when a player presses a key in the emulator.
        std::array<INPUT, 2> tap{};
        tap[0].type = INPUT_KEYBOARD;
        tap[0].ki.wVk = VK_SHIFT;
        tap[1] = tap[0];
        tap[1].ki.dwFlags = KEYEVENTF_KEYUP;
        const bool foreground = hwnd != nullptr && GetForegroundWindow() == hwnd;
        const UINT sent = SendInput(static_cast<UINT>(tap.size()), tap.data(), sizeof(INPUT));
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
        }
        log["inputAtEnd"] = {{"sent", sent == tap.size()}, {"foreground", foreground}};
    }
    if (options.leaveForegroundWindowMs > 0 && background) {
        log["foregroundHelper"] = {{"started", false}, {"skipped", "background (RV_LAUNCH_BACKGROUND)"}};
    } else if (options.leaveForegroundWindowMs > 0) {
        leaveForegroundWindow(options, log);
    }
    log["frames"] = frame;
    log["foreground"]["firstMs"] = firstForeground ? nlohmann::json(*firstForeground) : nlohmann::json(nullptr);
    log["foreground"]["samples"] = samples;
    log["foreground"]["atEnd"] = hwnd != nullptr && GetForegroundWindow() == hwnd;
    log["display"]["atEnd"] = describeMode(SDL_GetCurrentDisplayMode(display));
    log["windowEndMs"] = sinceStart();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    Options options;
    std::string error;
    if (!parse(args, options, error)) {
        if (error == "help") {
            std::fputs(kUsage.data(), stdout);
            return 0;
        }
        std::fprintf(stderr, "rv_fake_emulator: %s\n%s", error.c_str(), kUsage.data());
        return 2;
    }

    if (options.exitTimeoutMs > 0) {
        armExitTimeout(options.exitTimeoutMs);
    }

    nlohmann::json log;
    log["pid"] = GetCurrentProcessId();
    log["args"] = args;

    if (!options.printArgs.empty()) {
        std::vector<std::string> argv0;
        for (int i = 0; i < argc; ++i) {
            argv0.emplace_back(argv[i] != nullptr ? argv[i] : "");
        }
        writeJson(options.printArgs, nlohmann::json{{"payload", options.payload},
                                                    {"argv", argv0},
                                                    {"commandLine", rv::core::wideToUtf8Lossy(GetCommandLineW())}});
    }
    if (!options.printEnv.empty()) {
        nlohmann::json variables = nlohmann::json::object();
        if (wchar_t* block = GetEnvironmentStringsW(); block != nullptr) {
            for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
                const std::string text = rv::core::wideToUtf8Lossy(entry);
                const std::size_t equals = text.find('=', 1);
                if (equals != std::string::npos && rv::core::toLowerAscii(text.substr(0, 8)) == "rv_fake_") {
                    variables[text.substr(0, equals)] = text.substr(equals + 1);
                }
            }
            FreeEnvironmentStringsW(block);
        }
        writeJson(options.printEnv, variables);
    }
    if (!options.printCwd.empty()) {
        std::error_code ec;
        writeJson(options.printCwd,
                  nlohmann::json{{"cwd", rv::core::pathToUtf8Lossy(std::filesystem::current_path(ec))}});
    }
    if (!options.printConsole.empty()) {
        std::array<DWORD, 64> processes{};
        const DWORD count = GetConsoleProcessList(processes.data(), static_cast<DWORD>(processes.size()));
        // A console started with CREATE_NO_WINDOW has no window at all (GetConsoleWindow is null).
        const HWND consoleWindow = GetConsoleWindow();
        writeJson(options.printConsole,
                  nlohmann::json{{"hasConsole", count > 0},
                                 {"hasWindow", consoleWindow != nullptr},
                                 {"processes", std::vector<DWORD>(processes.begin(),
                                                                  processes.begin() + std::min<DWORD>(count, 64))}});
    }
    if (!options.stdoutText.empty()) {
        std::printf("%s\n", options.stdoutText.c_str());
        std::fprintf(stderr, "%s\n", options.stdoutText.c_str());
    }
    if (!options.outText.empty()) {
        std::printf("%s\n", options.outText.c_str());
    }
    if (!options.errText.empty()) {
        std::fprintf(stderr, "%s\n", options.errText.c_str());
    }
    std::fflush(stdout);
    std::fflush(stderr);

    if (options.foregroundWindowMs > 0) {
        runForegroundWindow(options, log);
        writeJson(options.log, log);
        return 0;
    }
    if (options.spawnChildren > 0 && !spawnChildren(options.spawnChildren)) {
        return 3;
    }

    if (options.spawnChildAndExit) {
        std::vector<std::string> childArgs;
        bool removed = false;
        bool inPayload = false;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string& arg = args[i];
            if (arg == "--") {
                inPayload = true;
            }
            if (!inPayload && !removed && arg == "--spawn-child-and-exit") {
                removed = true;
                continue;
            }
            if (!inPayload && arg == "--child-breakaway") {
                continue;
            }
            if (!inPayload && arg == "--spawn-children") {
                ++i; // and its count: the wrapper started them
                continue;
            }
            childArgs.push_back(arg);
        }
        const bool spawned = spawnChild(childArgs, options.childBreakaway);
        return spawned ? 0 : 3;
    }

    const Clock::time_point start = Clock::now();
    if (options.frames > 0) {
        runWindow(options, log);
    }
    if (options.sleepMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(options.sleepMs));
    }
    log["durationMs"] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    log["exit"] = options.crash ? "crash" : (options.hang ? "hang" : "code");
    log["exitCode"] = options.exitCode;
    writeJson(options.log, log);
    if (options.crash) {
        crashNow();
    }
    if (options.hang) {
        hang(options.hangLimitMs);
    }
    return static_cast<int>(options.exitCode);
}
