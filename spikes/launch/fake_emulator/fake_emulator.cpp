// rv_fake_emulator: a stand-in for an emulator in the launch tests (docs/spikes/i1-launch.md). It behaves like the
// cases the launcher must handle (a clean or failing exit, a crash, a hang, a wrapper that starts the real program and
// exits, a fullscreen window, an exclusive display mode, a window that grabs the foreground) and writes down what it
// saw: its arguments, environment and working directory (quoting and environment round trips), whether it got the
// foreground and which display mode it ran in.
//
//   rv_fake_emulator [options] [-- payload arguments...]
//
// Options are read up to the first "--"; everything after it is payload, echoed by --print-args and never interpreted.
// The program never terminates another process.

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
    bool spawnChildAndExit = false;
    bool childBreakaway = false;
    std::string printArgs;
    std::string printEnv;
    std::string printCwd;
    std::string stdoutText;
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
    "  --sleep-ms N             wait N ms (without a window) before exiting\n"
    "  --crash                  end with an access violation (0xC0000005) after the frames and the sleep\n"
    "  --hang                   never exit (the test harness ends the process)\n"
    "  --spawn-child-and-exit   start a copy of this program with the other options and exit at once\n"
    "  --child-breakaway        with --spawn-child-and-exit: the copy leaves the job (CREATE_BREAKAWAY_FROM_JOB)\n"
    "  --print-args FILE        write the payload, argv and the raw command line to FILE (JSON)\n"
    "  --print-env FILE         write the RV_FAKE_* environment variables to FILE (JSON)\n"
    "  --print-cwd FILE         write the current directory to FILE (JSON)\n"
    "  --stdout TEXT            print TEXT on standard output and standard error\n"
    "  --log FILE               write what the run saw (foreground, display modes, times) to FILE (JSON)\n"
    "  --color RRGGBB           window color\n";

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

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
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

/// Starts a copy of this program with `args` (its own options minus --spawn-child-and-exit) and returns at once.
bool spawnChild(const std::vector<std::string>& args, bool breakaway) {
    const std::filesystem::path self = rv::core::executablePath();
    const auto commandLine = rv::launcher::buildCommandLine(rv::core::pathToUtf8(self), args);
    if (!commandLine) {
        std::fprintf(stderr, "rv_fake_emulator: %s\n", commandLine.error().toString().c_str());
        return false;
    }
    const int units = MultiByteToWideChar(CP_UTF8, 0, commandLine->data(), static_cast<int>(commandLine->size()),
                                          nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(units), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, commandLine->data(), static_cast<int>(commandLine->size()), wide.data(), units);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    const DWORD flags = breakaway ? CREATE_BREAKAWAY_FROM_JOB : 0;
    if (!CreateProcessW(self.c_str(), wide.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &startup, &info)) {
        std::fprintf(stderr, "rv_fake_emulator: CreateProcessW failed with error %lu\n", GetLastError());
        return false;
    }
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    std::printf("rv_fake_emulator: started process %lu\n", info.dwProcessId);
    return true;
}

[[noreturn]] void crashNow() {
    // No error report dialog: the test harness is waiting for the exit code.
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    volatile int* volatile target = nullptr;
    *target = 0x0BADF00D;
    std::abort();
}

[[noreturn]] void hangForever() {
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
    if (options.stealFocus) {
        SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
    }
    const SDL_DisplayID display = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    log["display"]["desktop"] = describeMode(desktop);

    SDL_Window* window = SDL_CreateWindow("Retroverse fake emulator", 1280, 720, SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        log["error"] = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        SDL_Quit();
        return;
    }
    if (options.exclusive) {
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
    SDL_SetWindowFullscreen(window, true);
    SDL_ShowWindow(window);
    SDL_SyncWindow(window);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        log["error"] = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }
    SDL_SetRenderVSync(renderer, 1);
    const auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
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
        if (frame == 0 && options.stealFocus) {
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
    if (options.inputAtEnd) {
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
                                                    {"commandLine", narrow(GetCommandLineW())}});
    }
    if (!options.printEnv.empty()) {
        nlohmann::json variables = nlohmann::json::object();
        if (wchar_t* block = GetEnvironmentStringsW(); block != nullptr) {
            for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
                const std::string text = narrow(entry);
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
                  nlohmann::json{{"cwd", rv::core::pathToUtf8(std::filesystem::current_path(ec))}});
    }
    if (!options.stdoutText.empty()) {
        std::printf("%s\n", options.stdoutText.c_str());
        std::fprintf(stderr, "%s\n", options.stdoutText.c_str());
        std::fflush(stdout);
        std::fflush(stderr);
    }

    if (options.spawnChildAndExit) {
        std::vector<std::string> childArgs;
        bool removed = false;
        bool inPayload = false;
        for (const std::string& arg : args) {
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
        hangForever();
    }
    return static_cast<int>(options.exitCode);
}
