#include <rv/app/screen_room.hpp>

#include <algorithm>
#include <utility>

#include <rv/app/screen_policy.hpp>

namespace rv::app {
namespace {

/// The simulated times are sums of fixed steps: a dwell of exactly 0.3 s ends on its frame.
constexpr double kDwellTolerance = 1e-6;

} // namespace

ScreenRoomTracker::ScreenRoomTracker(std::optional<std::string> bound) : m_bound(std::move(bound)) {}

std::optional<std::string> ScreenRoomTracker::update(std::optional<std::string_view> eyeRoom, double nowSeconds) {
    if (!eyeRoom || eyeRoom->empty() || (m_bound && *m_bound == *eyeRoom)) {
        // A hall keeps the bound CRT; the bound room needs nothing; either way a dwell starts again from scratch.
        m_candidate.reset();
        return std::nullopt;
    }
    if (!m_candidate || *m_candidate != *eyeRoom) {
        m_candidate = std::string(*eyeRoom);
        m_since = nowSeconds;
    }
    if (nowSeconds - m_since + kDwellTolerance < kScreenDwellSeconds) {
        return std::nullopt;
    }
    m_bound = std::exchange(m_candidate, std::nullopt);
    return m_bound;
}

void ScreenRoomTracker::reset(std::optional<std::string> room) {
    m_bound = std::move(room);
    m_candidate.reset();
}

std::optional<std::vector<std::size_t>> rebindOrder(std::span<const std::string> bound,
                                                    std::span<const std::string> found) {
    if (bound.size() != found.size()) {
        return std::nullopt;
    }
    std::vector<std::size_t> order;
    order.reserve(bound.size());
    std::vector<bool> taken(found.size(), false);
    for (const std::string& id : bound) {
        const auto match = std::find(found.begin(), found.end(), id);
        if (match == found.end()) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(match - found.begin());
        // An id twice among the found: the second of the bound finds the first again.
        if (taken[index] || std::find(match + 1, found.end(), id) != found.end()) {
            return std::nullopt;
        }
        taken[index] = true;
        order.push_back(index);
    }
    return order;
}

} // namespace rv::app
