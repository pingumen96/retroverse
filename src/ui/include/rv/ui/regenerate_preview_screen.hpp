#pragma once

// The impact preview of a regeneration or an undo on assets/ui/regenerate_preview.rml (I4b step 6; ADR-0015 §5, §6):
// its data model "regenerate_preview" bound to a RegeneratePreviewModel (what it is about, where it stands, the counts,
// the conflicts, the buttons shown), the focus on the button the model names (the :focus style), the clicks on its
// buttons. The application gives the model the UI commands (NavInput) and forwards the pointer events.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <rv/core/expected.hpp>
#include <rv/ui/regenerate_preview_model.hpp>

union SDL_Event;

namespace rv::ui {

class UiView;

/// The preview's document in `view`'s context, over the documents loaded before it. It never takes the input from the
/// application: the model's focus is applied after each update. Main thread only; destroy it before its view.
class RegeneratePreviewScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<RegeneratePreviewScreen>> create(UiView& view);
    ~RegeneratePreviewScreen();
    RegeneratePreviewScreen(const RegeneratePreviewScreen&) = delete;
    RegeneratePreviewScreen& operator=(const RegeneratePreviewScreen&) = delete;

    /// Shows `model` (bound only when its revision changed), brought to the front when it was hidden. The model must
    /// stay alive while the screen shows it (afterUpdate() reads its focus).
    void show(const RegeneratePreviewModel& model);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// After UiView::update(): the focus on the button the model names.
    void afterUpdate();
    /// A mouse event while the panel shows: forwarded to the view (hover, clicks); true when it was a button event
    /// over the panel, which the world must not see. Other events, and any while it is hidden: false, not forwarded.
    bool handlePointer(const SDL_Event& event);
    /// The button a click pressed since the last call (RegeneratePreviewModel::click takes it).
    [[nodiscard]] std::optional<RegenerateButton> takeClicked() noexcept;
    /// The element that has RmlUi's focus ("" none): the model's, once afterUpdate() applied it.
    [[nodiscard]] std::string focusedElement() const;

private:
    struct Impl;

    explicit RegeneratePreviewScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
