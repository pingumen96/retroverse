#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <rv/core/expected.hpp>

namespace rv::ui {
class UiView;
} // namespace rv::ui

namespace rv::app {

/// What the world's notice says (world_notice_ui.hpp's keys and data).
struct WorldNoticeContent {
    std::string notice;
    /// The catalog's state, for world_notice::kNoCatalog.
    std::string state;
    /// The start room (its id, as --start-room gives it; empty without one), for world_notice::kNoGames.
    std::string startRoom;
    /// The room world_notice::kNeedsConfirmation offers to regenerate (I4b step 6's review): its kind ("console",
    /// "shared", "arcade") and, a console room's, its system's name.
    std::string roomKind;
    std::string roomTitle;

    bool operator==(const WorldNoticeContent&) const = default;
};

/// The world's notice (assets/ui/world_notice.rml, I3 step 10b): a band at the bottom of the view over the room, bound
/// to the RmlUi data model "world_notice" of `view`'s context. It takes neither the focus nor any input (no modal flag,
/// no focusable element), so the world and the quick catalog keep theirs. Main thread only; destroy it before its view.
class WorldNoticePanel {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<WorldNoticePanel>> create(ui::UiView& view);
    ~WorldNoticePanel();
    WorldNoticePanel(const WorldNoticePanel&) = delete;
    WorldNoticePanel& operator=(const WorldNoticePanel&) = delete;

    /// Shows `content` (an empty notice hides it); the same content again changes nothing.
    void show(const WorldNoticeContent& content);
    void hide();
    [[nodiscard]] bool visible() const noexcept;

private:
    struct Impl;

    explicit WorldNoticePanel(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::app
