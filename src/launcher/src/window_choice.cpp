#include <rv/launcher/window_choice.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <tuple>
#include <utility>

namespace rv::launcher {
namespace {

bool containsIgnoringCase(std::string_view text, std::string_view part) {
    if (part.empty()) {
        return true;
    }
    if (part.size() > text.size()) {
        return false;
    }
    const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
    for (std::size_t at = 0; at + part.size() <= text.size(); ++at) {
        bool same = true;
        for (std::size_t i = 0; i < part.size() && same; ++i) {
            same = lower(text[at + i]) == lower(part[i]);
        }
        if (same) {
            return true;
        }
    }
    return false;
}

bool matches(const WindowRule& rule, const WindowCandidate& window) {
    if (rule.classContains.empty() && rule.titleContains.empty()) {
        return false;
    }
    return containsIgnoringCase(window.className, rule.classContains) &&
           containsIgnoringCase(window.title, rule.titleContains);
}

/// Smaller is better: the rank, then the larger area, then the longer shown, then the lower handle.
auto orderKey(const WindowCandidate& window, WindowRank rank) {
    const long long area = static_cast<long long>(std::max(window.width, 0)) * std::max(window.height, 0);
    return std::make_tuple(static_cast<int>(rank), -area, -window.shownFor.count(), window.handle);
}

const WindowCandidate* find(std::span<const WindowCandidate> windows, std::uintptr_t handle) {
    const auto found = std::find_if(windows.begin(), windows.end(),
                                    [&](const WindowCandidate& window) { return window.handle == handle; });
    return found == windows.end() ? nullptr : &*found;
}

} // namespace

WindowRank rankWindow(const WindowCandidate& window, const WindowRules& rules) {
    if (window.cloaked || window.minimized) {
        return WindowRank::Excluded;
    }
    bool preferred = false;
    for (const WindowRule& rule : rules.rules) {
        if (!matches(rule, window)) {
            continue;
        }
        if (rule.effect == WindowRule::Effect::Skip) {
            return WindowRank::Excluded;
        }
        preferred = true;
    }
    if (preferred) {
        return WindowRank::Preferred;
    }
    if (window.console || window.width < rules.minWidth || window.height < rules.minHeight) {
        return WindowRank::LastResort;
    }
    return window.tool || window.owned ? WindowRank::Lesser : WindowRank::Normal;
}

std::optional<std::size_t> chooseGameWindow(std::span<const WindowCandidate> windows, const WindowRules& rules) {
    std::optional<std::size_t> best;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const WindowRank rank = rankWindow(windows[i], rules);
        if (rank == WindowRank::Excluded) {
            continue;
        }
        if (!best || orderKey(windows[i], rank) < orderKey(windows[*best], rankWindow(windows[*best], rules))) {
            best = i;
        }
    }
    return best;
}

WindowFollower::WindowFollower(WindowRules rules, std::chrono::milliseconds settle)
    : m_rules(std::move(rules)), m_settle(settle) {}

std::optional<std::uintptr_t> WindowFollower::update(std::span<const WindowCandidate> windows,
                                                     std::chrono::steady_clock::time_point now) {
    const auto best = chooseGameWindow(windows, m_rules);
    const std::uintptr_t bestHandle = best ? windows[*best].handle : 0;
    const WindowCandidate* followed = m_current != 0 ? find(windows, m_current) : nullptr;
    const bool followedUsable = followed != nullptr && rankWindow(*followed, m_rules) != WindowRank::Excluded;

    if (!followedUsable) {
        // None followed yet, or the followed one is gone: the best now, at once.
        m_challenger = 0;
        if (bestHandle == m_current) {
            return std::nullopt;
        }
        m_current = bestHandle;
        return m_current;
    }
    if (bestHandle == m_current) {
        m_challenger = 0;
        return std::nullopt;
    }
    if (bestHandle != m_challenger) {
        m_challenger = bestHandle;
        m_challengerSince = now;
    }
    if (now - m_challengerSince < m_settle) {
        return std::nullopt;
    }
    m_current = bestHandle;
    m_challenger = 0;
    return m_current;
}

} // namespace rv::launcher
