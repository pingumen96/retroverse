#include <rv/ui/focus_history.hpp>

#include <algorithm>
#include <iterator>
#include <utility>

namespace rv::ui {

void FocusHistory::onFocusMoved(const std::optional<FocusTarget>& from, std::string_view toRegion) {
    if (m_returningTo) {
        const bool arrived = *m_returningTo == toRegion;
        m_returningTo.reset();
        if (arrived) {
            return; // the move back() asked for
        }
    }
    if (!from || from->region == toRegion) {
        return; // the first focus of the screen, or a move within a region
    }
    const auto onPath = std::find_if(m_path.begin(), m_path.end(),
                                     [toRegion](const FocusTarget& entry) { return entry.region == toRegion; });
    if (onPath != m_path.end()) {
        m_path.erase(onPath, m_path.end()); // back on the path: the steps taken after leaving that region are undone
        return;
    }
    m_path.push_back(*from);
    if (m_path.size() > kMaxDepth) {
        m_path.erase(m_path.begin(), std::next(m_path.begin(), static_cast<std::ptrdiff_t>(m_path.size() - kMaxDepth)));
    }
}

std::optional<FocusTarget> FocusHistory::back() {
    if (m_path.empty()) {
        m_returningTo.reset();
        return std::nullopt;
    }
    FocusTarget target = std::move(m_path.back());
    m_path.pop_back();
    m_returningTo = target.region;
    return target;
}

const FocusTarget* FocusHistory::peek() const noexcept {
    return m_path.empty() ? nullptr : &m_path.back();
}

void FocusHistory::clear() noexcept {
    m_path.clear();
    m_returningTo.reset();
}

} // namespace rv::ui
