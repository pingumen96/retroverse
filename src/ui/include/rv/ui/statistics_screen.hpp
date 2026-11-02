#pragma once

// The statistics screen on assets/ui/statistics.rml (ADR-0018 §9): its data model "statistics" bound to a StatisticsModel,
// the body scrolled by the UI commands, the clicks on the sections and on Chiudi. The application gives the model the UI
// commands and forwards the pointer events.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/core/expected.hpp>
#include <rv/ui/game_info_model.hpp>
#include <rv/ui/statistics_model.hpp>

union SDL_Event;

namespace rv::ui {

class UiView;

/// A click: on a section, or on Chiudi.
struct StatisticsClick {
    std::optional<StatisticsTab> tab;
    bool close = false;
};

/// The document in `view`'s context, over the documents loaded before it. It never takes RmlUi's focus. Main thread only;
/// destroy it before its view.
class StatisticsScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<StatisticsScreen>> create(UiView& view);
    ~StatisticsScreen();
    StatisticsScreen(const StatisticsScreen&) = delete;
    StatisticsScreen& operator=(const StatisticsScreen&) = delete;

    /// Shows `model` (bound only when its revision changed; another section scrolls back to the top), brought to the
    /// front when it was hidden. It must stay alive while the screen shows it.
    void show(const StatisticsModel& model);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// Scrolls the body (the scrolling steps of InfoStep; others do nothing). True when it moved.
    bool scroll(InfoStep step);
    /// A mouse event while it shows: forwarded to the view; true when it was a button event over the panel.
    bool handlePointer(const SDL_Event& event);
    /// The click since the last call.
    [[nodiscard]] std::optional<StatisticsClick> takeClicked() noexcept;
    /// Elements of the panel whose text does not fit its box as shown now.
    [[nodiscard]] std::uint32_t textCut() const;
    /// The text of the line that names the hours, as the document shows it ("" when it does not show).
    [[nodiscard]] std::string hoursLabel() const;
    /// What the recent games shown say of those with no session, in order.
    [[nodiscard]] std::vector<std::string> recentNotes() const;

private:
    struct Impl;

    explicit StatisticsScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
