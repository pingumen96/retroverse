#pragma once

// GPU-free (target rv_devui_model): no SDL, OpenGL or Dear ImGui header is included.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <spdlog/common.h>

namespace rv::devui {

/// The overlay's copy of the in-memory log (core::recentLogLines) for the Log panel: the most recent `capacity` lines
/// in a ring, each with its level and in a short form. refresh() fetches only the lines logged since the previous call
/// (core::loggedMessageCount), so drawing the panel every frame copies nothing while the log is quiet; the strings of
/// dropped lines keep their storage for the new ones.
class LogView {
public:
    /// One log line.
    struct Line {
        /// The level of the line (core::parseLogLine); info for text that does not follow the log pattern.
        spdlog::level::level_enum level = spdlog::level::info;
        /// "HH:MM:SS.mmm  <message>" (the date and thread left out), or the whole text, without its trailing line
        /// break, when it does not follow the log pattern.
        std::string text;
    };

    /// Lines kept: the size of the logger's in-memory ring (core::LogConfig::ringBufferLines).
    static constexpr std::size_t kDefaultCapacity = 512;

    /// `capacity` (at least 1) lines are kept.
    explicit LogView(std::size_t capacity = kDefaultCapacity);

    /// Appends the lines the logger wrote since the previous refresh (all of them the first time, at most the
    /// capacity). Returns true when lines were added. After logging was re-initialized (the count went back), the view
    /// starts over.
    bool refresh();

    /// Appends formatted log lines, oldest first, dropping the oldest lines beyond the capacity: refresh() feeds the
    /// view through it.
    void append(std::span<const std::string> lines);

    /// Lines kept, oldest first.
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] const Line& line(std::size_t index) const;
    [[nodiscard]] std::size_t capacity() const noexcept { return m_lines.size(); }

private:
    void push(std::string_view line);

    std::vector<Line> m_lines;
    std::size_t m_first = 0;
    std::size_t m_size = 0;
    /// core::loggedMessageCount() at the previous refresh.
    std::uint64_t m_seen = 0;
};

} // namespace rv::devui
