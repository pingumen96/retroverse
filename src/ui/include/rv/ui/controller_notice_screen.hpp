#pragma once

// "Controller scollegato" (FR-029, ADR-0018 §7; assets/ui/controller_notice.rml): a small notice in the view's context, over
// everything that shows, so that a pad lost in a menu says so too. It takes no input and never the focus.

#include <memory>

#include <rv/core/expected.hpp>

namespace rv::ui {

class UiView;

class ControllerNoticeScreen {
public:
    /// Loads the document, hidden. Errors: IoError (details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<ControllerNoticeScreen>> create(UiView& view);
    ~ControllerNoticeScreen();
    ControllerNoticeScreen(const ControllerNoticeScreen&) = delete;
    ControllerNoticeScreen& operator=(const ControllerNoticeScreen&) = delete;

    /// Shown, in front of what shows, or hidden.
    void setShown(bool shown);

private:
    struct Impl;

    explicit ControllerNoticeScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
