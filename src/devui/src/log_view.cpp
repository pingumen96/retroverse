#include <rv/devui/log_view.hpp>

#include <algorithm>
#include <optional>

#include <rv/core/log.hpp>

namespace rv::devui {

LogView::LogView(std::size_t capacity) : m_lines(std::max<std::size_t>(capacity, 1)) {}

bool LogView::refresh() {
    const std::uint64_t logged = core::loggedMessageCount();
    if (logged < m_seen) {
        // Logging was configured again: its lines are another log.
        m_first = 0;
        m_size = 0;
        m_seen = 0;
    }
    if (logged == m_seen) {
        return false;
    }
    // The newest lines. With other threads logging, a line written between the two calls can take the place of this
    // batch's oldest line and show again in the next batch: a cosmetic glitch of the overlay's copy, never of the log.
    const auto added = static_cast<std::size_t>(std::min<std::uint64_t>(logged - m_seen, capacity()));
    m_seen = logged;
    append(core::recentLogLines(added));
    return true;
}

void LogView::append(std::span<const std::string> lines) {
    for (const std::string& line : lines) {
        push(line);
    }
}

const LogView::Line& LogView::line(std::size_t index) const {
    return m_lines.at((m_first + index) % m_lines.size());
}

void LogView::push(std::string_view line) {
    Line* slot = nullptr;
    if (m_size < m_lines.size()) {
        slot = &m_lines[(m_first + m_size) % m_lines.size()];
        ++m_size;
    } else {
        // Full: the oldest line makes room, and its string keeps its storage for the new text.
        slot = &m_lines[m_first];
        m_first = (m_first + 1) % m_lines.size();
    }
    if (const std::optional<core::LogLineFields> fields = core::parseLogLine(line)) {
        slot->level = fields->level;
        slot->text.assign(fields->time);
        slot->text.append("  ");
        slot->text.append(fields->message);
        return;
    }
    while (line.ends_with('\n') || line.ends_with('\r')) {
        line.remove_suffix(1);
    }
    slot->level = spdlog::level::info;
    slot->text.assign(line);
}

} // namespace rv::devui
