#pragma once

// Which window of a launched emulator is the game's (I11 step 11, ADR-0020 §2): the one VR captures onto the room's
// CRT and gives the foreground. An emulator may show a splash or an update dialog first, a game list beside a separate
// render window, a console window, or make its window again when it switches renderer or display mode; the window
// followed must be the game's through all of it. GPU-free and Win32-free: the runner describes the tree's visible
// top-level windows as WindowCandidates (rv_platform_win), chooseGameWindow() ranks them, and a WindowFollower keeps
// the choice steady across checks, switching only when a better window has stayed better for a moment or the followed
// one is gone.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rv::launcher {

/// A visible top-level window of the launched tree, as the runner sees it.
struct WindowCandidate {
    /// The window (an HWND as an integer), never 0.
    std::uintptr_t handle = 0;
    std::uint32_t processId = 0;
    /// Its window class and title, UTF-8.
    std::string className;
    std::string title;
    /// Its client area in pixels.
    int width = 0;
    int height = 0;
    bool tool = false;      ///< WS_EX_TOOLWINDOW
    bool owned = false;     ///< it has an owner window: a dialog or a palette of another window
    bool console = false;   ///< a console host's window (ConsoleWindowClass, a pseudo console, Windows Terminal)
    bool cloaked = false;   ///< DWM cloaks it: not on screen (another virtual desktop, a suspended app)
    bool minimized = false; ///< iconic: nothing to capture until it is restored
    /// How long it has been visible, since the runner first saw it.
    std::chrono::milliseconds shownFor{0};

    bool operator==(const WindowCandidate&) const = default;
};

/// A rule of an emulator's profile (ADR-0020 §1): a window whose class or title holds a text (case-insensitive, ASCII)
/// is preferred over every other, or never followed. Both texts given: both must match; neither: no window matches.
struct WindowRule {
    enum class Effect : std::uint8_t { Prefer, Skip };
    std::string classContains;
    std::string titleContains;
    Effect effect = Effect::Prefer;

    bool operator==(const WindowRule&) const = default;
};

/// How a launch's windows are ranked.
struct WindowRules {
    std::vector<WindowRule> rules;
    /// Smaller client areas rank last: splashes, prompts, palettes.
    int minWidth = 200;
    int minHeight = 150;

    bool operator==(const WindowRules&) const = default;
};

/// The ranks a window can take, best first.
enum class WindowRank : std::uint8_t {
    Preferred, ///< a Prefer rule matches it
    Normal,    ///< a window of its own, large enough
    Lesser,    ///< a tool window or an owned one, large enough
    LastResort, ///< too small, or a console's window: only when nothing else is shown
    Excluded,  ///< a Skip rule matches it, it is cloaked or minimized
};

/// The rank of one window under `rules`.
[[nodiscard]] WindowRank rankWindow(const WindowCandidate& window, const WindowRules& rules);

/// The index of the game's window among `windows`: the best rank, then the largest client area, then the one shown
/// longest, then the lowest handle (stable when everything ties). Nothing when every window is excluded.
[[nodiscard]] std::optional<std::size_t> chooseGameWindow(std::span<const WindowCandidate> windows,
                                                          const WindowRules& rules);

/// The followed window across the runner's checks.
class WindowFollower {
public:
    /// A better window must stay better for `settle` before the choice moves to it (a window being resized, two windows
    /// shown in turn, never make it flap).
    explicit WindowFollower(WindowRules rules = {}, std::chrono::milliseconds settle = std::chrono::milliseconds(300));

    /// One check: the tree's visible windows now. Returns the newly followed window when the choice changed (0: none
    /// any more), nothing when it stays.
    ///  - With none followed, the best window is followed at once.
    ///  - The followed window still there and not excluded stays until another has ranked better for `settle`.
    ///  - The followed window gone or excluded (closed, minimized, cloaked): the best other window at once, else 0.
    std::optional<std::uintptr_t> update(std::span<const WindowCandidate> windows,
                                         std::chrono::steady_clock::time_point now);

    /// The followed window, 0 for none.
    [[nodiscard]] std::uintptr_t current() const noexcept { return m_current; }
    [[nodiscard]] const WindowRules& rules() const noexcept { return m_rules; }

private:
    WindowRules m_rules;
    std::chrono::milliseconds m_settle;
    std::uintptr_t m_current = 0;
    std::uintptr_t m_challenger = 0;
    std::chrono::steady_clock::time_point m_challengerSince{};
};

} // namespace rv::launcher
