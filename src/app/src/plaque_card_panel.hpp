#pragma once

#include <memory>

#include <rv/app/plaque_card_ui.hpp>
#include <rv/core/expected.hpp>

namespace rv::ui {
class UiView;
} // namespace rv::ui

namespace rv::app {

/// The history plaque's card (assets/ui/room_plaque.rml, I11 step 5; ADR-0019 §6): what a console room's plaque says,
/// in full, over the room, bound to the RmlUi data model "room_plaque" of `view`'s context. It takes no focus: the
/// application keeps the input and closes it on Back or Conferma (ProductUi). Main thread only; destroy it before its
/// view.
class PlaqueCardPanel {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<PlaqueCardPanel>> create(ui::UiView& view);
    ~PlaqueCardPanel();
    PlaqueCardPanel(const PlaqueCardPanel&) = delete;
    PlaqueCardPanel& operator=(const PlaqueCardPanel&) = delete;

    void show(const PlaqueCardContent& content);
    void hide();
    /// Scrolls the text by `steps` lines (negative: up), within its region.
    void scroll(int steps);
    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] const PlaqueCardContent& content() const noexcept;

private:
    struct Impl;

    explicit PlaqueCardPanel(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
