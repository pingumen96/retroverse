#pragma once

// The media coverage screen on assets/ui/coverage.rml (ADR-0018 §14): its data model "coverage" bound to a CoverageModel,
// the focus the model's, a system's details scrolled by the UI commands, the clicks on the populations, the rows and
// Chiudi. The application gives the model the UI commands and forwards the pointer events.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <rv/core/expected.hpp>
#include <rv/ui/coverage_model.hpp>
#include <rv/ui/game_info_model.hpp>

union SDL_Event;

namespace rv::ui {

class UiView;

/// The document in `view`'s context, over the documents loaded before it. It never keeps RmlUi's focus once hidden.
/// Main thread only; destroy it before its view.
class CoverageScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<CoverageScreen>> create(UiView& view);
    ~CoverageScreen();
    CoverageScreen(const CoverageScreen&) = delete;
    CoverageScreen& operator=(const CoverageScreen&) = delete;

    /// Shows `model` (bound only when its revision changed), brought to the front when it was hidden. It must stay alive
    /// while the screen shows it.
    void show(const CoverageModel& model);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// After the view's update: the model's focus applied to the document (the row scrolled into view).
    void afterUpdate();
    /// Scrolls a system's details (the scrolling steps of InfoStep; others do nothing). True when it moved.
    bool scroll(InfoStep step);
    /// A mouse event while it shows: forwarded to the view; true when it was a button event over the panel.
    bool handlePointer(const SDL_Event& event);
    /// The click since the last call.
    [[nodiscard]] std::optional<CoverageClick> takeClicked() noexcept;
    /// Elements of the panel whose text does not fit its box as shown now, and parts that run past the panel.
    [[nodiscard]] std::uint32_t textCut() const;
    /// The element that has RmlUi's focus in the document ("" none).
    [[nodiscard]] std::string focusedElement() const;

private:
    struct Impl;

    explicit CoverageScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
