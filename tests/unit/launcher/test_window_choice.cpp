// The game's window among a launched tree's (I11 step 11, ADR-0020 §2): the ranks, the order within a rank, the rules
// of a profile, and the follower's steadiness: a splash then the game, a game list beside a render window, a console
// window first, a window made again, a window minimized.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rv/launcher/window_choice.hpp>

using rv::launcher::WindowCandidate;
using rv::launcher::WindowFollower;
using rv::launcher::WindowRank;
using rv::launcher::WindowRule;
using rv::launcher::WindowRules;
using namespace std::chrono_literals;

namespace {

WindowCandidate window(std::uintptr_t handle, int width, int height, std::string className = "SDL_app",
                       std::string title = "Game") {
    WindowCandidate candidate;
    candidate.handle = handle;
    candidate.processId = 100;
    candidate.className = std::move(className);
    candidate.title = std::move(title);
    candidate.width = width;
    candidate.height = height;
    return candidate;
}

using Clock = std::chrono::steady_clock;

} // namespace

TEST_CASE("launcher/window: each window's rank", "[launcher][window]") {
    const WindowRules rules;
    CHECK(rankWindow(window(1, 640, 480), rules) == WindowRank::Normal);
    CHECK(rankWindow(window(1, 199, 480), rules) == WindowRank::LastResort);
    CHECK(rankWindow(window(1, 640, 149), rules) == WindowRank::LastResort);
    WindowCandidate tool = window(1, 640, 480);
    tool.tool = true;
    CHECK(rankWindow(tool, rules) == WindowRank::Lesser);
    WindowCandidate owned = window(1, 640, 480);
    owned.owned = true;
    CHECK(rankWindow(owned, rules) == WindowRank::Lesser);
    WindowCandidate console = window(1, 1200, 600, "ConsoleWindowClass");
    console.console = true;
    CHECK(rankWindow(console, rules) == WindowRank::LastResort);
    WindowCandidate minimized = window(1, 640, 480);
    minimized.minimized = true;
    CHECK(rankWindow(minimized, rules) == WindowRank::Excluded);
    WindowCandidate cloaked = window(1, 640, 480);
    cloaked.cloaked = true;
    CHECK(rankWindow(cloaked, rules) == WindowRank::Excluded);

    WindowRules profile;
    profile.rules.push_back(WindowRule{"", "| JIT64", WindowRule::Effect::Prefer});
    profile.rules.push_back(WindowRule{"QWidget", "Memory", WindowRule::Effect::Skip});
    CHECK(rankWindow(window(1, 300, 200, "Qt5QWindowIcon", "Dolphin 2412 | jit64 DC | OpenGL"), profile) ==
          WindowRank::Preferred); // case-insensitive
    CHECK(rankWindow(window(1, 900, 700, "Qt5QWindowIconQWidget", "Memory viewer"), profile) == WindowRank::Excluded);
    CHECK(rankWindow(window(1, 900, 700, "Qt5QWindowIconQWidget", "Dolphin 2412"), profile) == WindowRank::Normal);
    // A rule with neither text matches nothing.
    WindowRules empty;
    empty.rules.push_back(WindowRule{"", "", WindowRule::Effect::Skip});
    CHECK(rankWindow(window(1, 640, 480), empty) == WindowRank::Normal);
}

TEST_CASE("launcher/window: the game's window is the best rank, then the largest, then the oldest",
          "[launcher][window]") {
    const WindowRules rules;
    std::vector<WindowCandidate> windows{window(10, 320, 180), window(11, 1280, 720), window(12, 640, 360)};
    CHECK(chooseGameWindow(windows, rules) == 1u);

    // A console window, however large, gives way to any window of the emulator's own.
    windows = {window(10, 1920, 1080, "ConsoleWindowClass"), window(11, 640, 360)};
    windows[0].console = true;
    CHECK(chooseGameWindow(windows, rules) == 1u);
    // Alone, it is still followed (a console emulator).
    windows.pop_back();
    CHECK(chooseGameWindow(windows, rules) == 0u);

    // A tool window alone is followed (the fake emulator's background window); beside a normal one it is not.
    std::vector<WindowCandidate> tools{window(20, 1280, 720), window(21, 320, 180)};
    tools[0].tool = true;
    CHECK(chooseGameWindow(tools, rules) == 1u);
    tools.pop_back();
    CHECK(chooseGameWindow(tools, rules) == 0u);

    // Ties: the one shown longest, then the lower handle.
    std::vector<WindowCandidate> ties{window(31, 640, 360), window(30, 640, 360)};
    ties[0].shownFor = 2000ms;
    CHECK(chooseGameWindow(ties, rules) == 0u);
    ties[0].shownFor = 0ms;
    CHECK(chooseGameWindow(ties, rules) == 1u);

    // Nothing to follow.
    std::vector<WindowCandidate> none{window(40, 640, 360)};
    none[0].minimized = true;
    CHECK_FALSE(chooseGameWindow(none, rules));
    CHECK_FALSE(chooseGameWindow(std::vector<WindowCandidate>{}, rules));
}

TEST_CASE("launcher/window: the follower moves only to a window that stays better, or when its window goes",
          "[launcher][window]") {
    WindowFollower follower;
    const Clock::time_point start = Clock::now();

    SECTION("a splash, then the game beside it, then the splash closes") {
        std::vector<WindowCandidate> windows{window(1, 260, 160)};
        CHECK(follower.update(windows, start) == 1u); // the first window at once
        windows.push_back(window(2, 1280, 720));
        CHECK_FALSE(follower.update(windows, start + 100ms)); // better, not yet for long enough
        CHECK_FALSE(follower.update(windows, start + 300ms));
        CHECK(follower.update(windows, start + 400ms) == 2u); // better for 300 ms
        CHECK(follower.current() == 2u);
        windows.erase(windows.begin());
        CHECK_FALSE(follower.update(windows, start + 500ms));
    }
    SECTION("a game list and a render window: the larger, or the one a rule prefers") {
        std::vector<WindowCandidate> windows{window(1, 800, 600, "Qt", "Dolphin 2412"),
                                             window(2, 640, 528, "Qt", "Dolphin 2412 | JIT64 DC | Game")};
        CHECK(follower.update(windows, start) == 1u);
        WindowRules rules;
        rules.rules.push_back(WindowRule{"", "| JIT64", WindowRule::Effect::Prefer});
        WindowFollower dolphin(rules);
        CHECK(dolphin.update(windows, start) == 2u);
    }
    SECTION("a window made again while another stays: the new one at once") {
        std::vector<WindowCandidate> windows{window(1, 320, 200), window(2, 1280, 720)};
        CHECK(follower.update(windows, start) == 2u);
        windows[1] = window(3, 1280, 720); // 2 destroyed, 3 made in its place
        CHECK(follower.update(windows, start + 100ms) == 3u);
    }
    SECTION("the followed window minimized: another, else none; restored: back at once") {
        std::vector<WindowCandidate> windows{window(1, 1280, 720)};
        CHECK(follower.update(windows, start) == 1u);
        windows[0].minimized = true;
        CHECK(follower.update(windows, start + 100ms) == 0u);
        CHECK_FALSE(follower.update(windows, start + 200ms));
        windows[0].minimized = false;
        CHECK(follower.update(windows, start + 300ms) == 1u);
    }
    SECTION("two windows taking turns never make it flap") {
        std::vector<WindowCandidate> windows{window(1, 800, 600), window(2, 640, 480)};
        CHECK(follower.update(windows, start) == 1u);
        for (int i = 1; i <= 10; ++i) {
            // 2 larger for 200 ms, then 1 again: never long enough.
            windows[1].width = i % 2 == 1 ? 1024 : 640;
            CHECK_FALSE(follower.update(windows, start + std::chrono::milliseconds(200 * i)));
        }
        CHECK(follower.current() == 1u);
    }
    SECTION("every window gone") {
        std::vector<WindowCandidate> windows{window(1, 1280, 720)};
        CHECK(follower.update(windows, start) == 1u);
        CHECK(follower.update(std::vector<WindowCandidate>{}, start + 100ms) == 0u);
        CHECK_FALSE(follower.update(std::vector<WindowCandidate>{}, start + 200ms));
    }
}
