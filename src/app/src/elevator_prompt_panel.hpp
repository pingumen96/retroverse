#pragma once

#include <memory>
#include <string>

#include <rv/core/expected.hpp>

namespace rv::ui {
class UiView;
} // namespace rv::ui

namespace rv::app {

/// The elevator's prompt (assets/ui/elevator_prompt.rml, I4 step 7b; ADR-0013 §10): one line low in the view while the
/// walker stands in front of a floor's elevator, naming the key that opens the directory, bound to the RmlUi data model
/// "elevator_prompt" of `view`'s context. Like the world's notice it takes neither the focus nor any input. Main thread
/// only; destroy it before its view.
class ElevatorPromptPanel {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<ElevatorPromptPanel>> create(ui::UiView& view);
    ~ElevatorPromptPanel();
    ElevatorPromptPanel(const ElevatorPromptPanel&) = delete;
    ElevatorPromptPanel& operator=(const ElevatorPromptPanel&) = delete;

    /// Shows the prompt naming `key`; the same key again changes nothing.
    void show(const std::string& key, bool totem = false);
    void hide();
    [[nodiscard]] bool visible() const noexcept;

private:
    struct Impl;

    explicit ElevatorPromptPanel(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
