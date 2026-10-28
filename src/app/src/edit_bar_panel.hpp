#pragma once

#include <memory>

#include <rv/app/edit_bar_ui.hpp>
#include <rv/core/expected.hpp>

namespace rv::ui {
class UiView;
} // namespace rv::ui

namespace rv::app {

/// The editor's bar (assets/ui/edit_bar.rml, I4b step 3; ADR-0015 §4): the band at the bottom of the view while a room is
/// edited, its menu above it, bound to the RmlUi data model "edit_bar" of `view`'s context. Like the world's notice it
/// takes neither the focus nor any input (the world reads the editor's actions). Main thread only; destroy it before
/// its view.
class EditBarPanel {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<EditBarPanel>> create(ui::UiView& view);
    ~EditBarPanel();
    EditBarPanel(const EditBarPanel&) = delete;
    EditBarPanel& operator=(const EditBarPanel&) = delete;

    /// Shows `content`; the same content again changes nothing.
    void show(const EditBarContent& content);
    void hide();
    [[nodiscard]] bool visible() const noexcept;

private:
    struct Impl;

    explicit EditBarPanel(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
