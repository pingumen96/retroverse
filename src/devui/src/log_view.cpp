#include <rv/devui/log_view.hpp>

#include <algorithm>
#include <optional>

#include <rv/core/log.hpp>

namespace rv::devui {

LogView::LogView(std::size_t capacity) : m_lines(std::max<std::size_t>(capacity, 1)) {}

bool LogView::refresh() {
    // The lines and the count are read together, under the in-memory log's lock (I3 step 13r): a line another thread
    // logs meanwhile comes with the next batch, never twice.
    core::LogLinesSince batch = core::logLinesSince(m_generation, m_seen, capacity());
    if (batch.generation != m_generation) {
        // Logging was configured again: its lines are another log.
        m_first = 0;
        m_size = 0;
        m_generation = batch.generation;
    }
    m_seen = batch.count;
    if (batch.lines.empty()) {
        return false;
    }
    append(batch.lines);
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
