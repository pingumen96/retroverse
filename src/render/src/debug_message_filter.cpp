#include <rv/render/debug_message_filter.hpp>

#include <algorithm>
#include <cstddef>

namespace rv::gl {
namespace {

constexpr std::size_t indexOf(DebugSeverity severity) noexcept {
    return static_cast<std::size_t>(severity);
}

} // namespace

std::string_view toString(DebugSeverity severity) noexcept {
    switch (severity) {
    case DebugSeverity::Notification:
        return "notification";
    case DebugSeverity::Low:
        return "low";
    case DebugSeverity::Medium:
        return "medium";
    case DebugSeverity::High:
        return "high";
    }
    return "unknown";
}

DebugMessageFilter::DebugMessageFilter(std::span<const std::uint32_t> ignoredIds, std::uint32_t repeatLimit)
    : m_ignoredIds(ignoredIds.begin(), ignoredIds.end()), m_repeatLimit(std::max<std::uint32_t>(repeatLimit, 1)) {
    std::sort(m_ignoredIds.begin(), m_ignoredIds.end());
}

DebugMessageAction DebugMessageFilter::classify(const DebugMessageKey& key, DebugSeverity severity) {
    if (severity == DebugSeverity::Notification ||
        std::binary_search(m_ignoredIds.begin(), m_ignoredIds.end(), key.id)) {
        return DebugMessageAction::Drop;
    }
    ++m_counts[indexOf(severity)];
    const std::uint64_t occurrence = ++m_occurrences[key];
    if (occurrence <= m_repeatLimit) {
        return DebugMessageAction::Log;
    }
    return occurrence == std::uint64_t{m_repeatLimit} + 1 ? DebugMessageAction::StartSuppressing
                                                           : DebugMessageAction::Suppress;
}

std::uint64_t DebugMessageFilter::count(DebugSeverity atLeast) const noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = indexOf(atLeast); i < m_counts.size(); ++i) {
        total += m_counts[i];
    }
    return total;
}

std::vector<DebugMessageFilter::SuppressedMessages> DebugMessageFilter::suppressed() const {
    std::vector<SuppressedMessages> result;
    for (const auto& [key, occurrences] : m_occurrences) {
        if (occurrences > m_repeatLimit) {
            result.push_back(SuppressedMessages{key, occurrences - m_repeatLimit});
        }
    }
    return result;
}

} // namespace rv::gl
