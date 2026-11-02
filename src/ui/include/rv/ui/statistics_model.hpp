#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The statistics screen (ADR-0018 §9; assets/ui/statistics.rml through ui::StatisticsScreen): sections of what was played
// in Retroverse, the hours always called estimated. The figures are the catalog worker's, in text as they show; every
// word is the document's. Left, Right and the filters change the section, Up, Down and the pages scroll it (the screen's,
// infoStepOf()'s steps), Back closes. A click on a section goes to it; the wheel scrolls, the ray's too.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

namespace statistics {
inline constexpr std::string_view kDocumentPath = "ui/statistics.rml";
inline constexpr std::string_view kDataModel = "statistics";
inline constexpr std::string_view kPanelId = "statistics-panel";
/// The region that scrolls.
inline constexpr std::string_view kBodyId = "st-body";
/// The summary's line that names the hours: what the tests read.
inline constexpr std::string_view kHoursLabelId = "st-hours-label";
} // namespace statistics

enum class StatisticsState : std::uint8_t {
    Loading,     ///< asked of the catalog
    Listed,      ///< its figures show
    Empty,       ///< nothing played yet
    Unavailable, ///< no catalog is open, or it cannot be read
};
/// "loading", "listed", "empty", "unavailable".
[[nodiscard]] std::string_view toString(StatisticsState state) noexcept;

enum class StatisticsTab : std::uint8_t { Summary, MostPlayed, Recent, Systems, Statuses };
inline constexpr std::array<StatisticsTab, 5> kStatisticsTabs = {StatisticsTab::Summary, StatisticsTab::MostPlayed,
                                                                 StatisticsTab::Recent, StatisticsTab::Systems,
                                                                 StatisticsTab::Statuses};
/// "summary", "most-played", "recent", "systems", "statuses" (the click event's argument).
[[nodiscard]] std::string_view toString(StatisticsTab tab) noexcept;
[[nodiscard]] std::optional<StatisticsTab> statisticsTabFromName(std::string_view name) noexcept;

/// A game of a list.
struct StatisticsGame {
    std::string title;
    /// The system as people read it.
    std::string system;
    /// Estimated hours as they show ("12,5"), the sessions, the day it was last played ("" unknown).
    std::string hours;
    int sessions = 0;
    std::string date;
    /// catalog::toString(GameStatus).
    std::string status;

    bool operator==(const StatisticsGame&) const = default;
};

struct StatisticsSystem {
    std::string name;
    std::string hours;
    int sessions = 0;
    int attempts = 0;
    int games = 0;

    bool operator==(const StatisticsSystem&) const = default;
};

struct StatisticsStatus {
    /// catalog::toString(GameStatus).
    std::string status;
    int games = 0;
    int sessions = 0;
    std::string hours;

    bool operator==(const StatisticsStatus&) const = default;
};

/// What the screen shows.
struct StatisticsContent {
    /// Nothing was played yet.
    bool empty = true;
    int sessions = 0;
    std::string hours = "0";
    int gamesPlayed = 0;
    int games = 0;
    int crashed = 0;
    int leftRunning = 0;
    std::vector<StatisticsGame> mostPlayed;
    std::vector<StatisticsGame> recent;
    std::vector<StatisticsSystem> systems;
    std::vector<StatisticsStatus> statuses;

    bool operator==(const StatisticsContent&) const = default;
};

/// What the screen asks of the application.
enum class StatisticsCommand : std::uint8_t {
    Close, ///< it closed itself
    Load,  ///< the figures
};

class StatisticsModel {
public:
    /// Opens on the summary, asking for the figures (takeRequest()).
    void open();
    void close() noexcept;

    /// The figures are asked again on the next frame (a catalog still opening).
    void prepare() { m_request = StatisticsCommand::Load; }
    /// What the application must do next, once.
    [[nodiscard]] std::optional<StatisticsCommand> takeRequest() noexcept;
    /// A UI command while open: the section, Back. The scrolling is the screen's.
    [[nodiscard]] std::optional<StatisticsCommand> apply(UiCommand command);
    /// A click on a section.
    void click(StatisticsTab tab);
    /// A click on Chiudi.
    [[nodiscard]] std::optional<StatisticsCommand> clickClose();

    void setContent(StatisticsContent content);
    /// No figures can be had.
    void setUnavailable();

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    [[nodiscard]] StatisticsState state() const noexcept { return m_state; }
    [[nodiscard]] StatisticsTab tab() const noexcept { return m_tab; }
    [[nodiscard]] const StatisticsContent& content() const noexcept { return m_content; }
    /// Every change of what shows increments it.
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

private:
    void step(int delta);

    bool m_open = false;
    StatisticsState m_state = StatisticsState::Loading;
    StatisticsTab m_tab = StatisticsTab::Summary;
    StatisticsContent m_content;
    std::optional<StatisticsCommand> m_request;
    std::uint64_t m_revision = 0;
};

/// `milliseconds` of play as estimated hours show them, in Italian: one decimal, a comma ("12,5"); "0" for nothing; "0,1"
/// for anything less than that, so that a short game never reads as none. Never rounds up across what it holds: a negative
/// time reads "0".
[[nodiscard]] std::string estimatedHours(std::int64_t milliseconds);

} // namespace rv::ui
