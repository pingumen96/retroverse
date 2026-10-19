#pragma once

#include <memory>

#include <glm/vec2.hpp>

#include <rv/app/world_label_ui.hpp>
#include <rv/core/expected.hpp>

namespace rv::ui {
class UiView;
} // namespace rv::ui

namespace rv::app {

/// The hover's title label (assets/ui/world_label.rml, I3 step 13a; ADR-0012 §3): the title of the item outlined in the
/// room, centred just above it, bound to the RmlUi data model "world_label" of `view`'s context. Like the world's
/// notice it takes neither the focus nor any input. Main thread only; destroy it before its view.
class WorldLabelPanel {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<WorldLabelPanel>> create(ui::UiView& view);
    ~WorldLabelPanel();
    WorldLabelPanel(const WorldLabelPanel&) = delete;
    WorldLabelPanel& operator=(const WorldLabelPanel&) = delete;

    /// Shows `content` in a view of `viewSize` pixels: the label's band the view's width, centred on content.x, its
    /// bottom world_label::kGapPixels above content.y. The same content again changes nothing.
    void show(const WorldLabelContent& content, glm::ivec2 viewSize);
    void hide();
    [[nodiscard]] bool visible() const noexcept;

private:
    struct Impl;

    explicit WorldLabelPanel(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
