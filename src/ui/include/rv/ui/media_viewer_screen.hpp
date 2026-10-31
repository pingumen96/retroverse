#pragma once

// The media viewer on assets/ui/media_viewer.rml (I3 step 14b; ADR-0012 §5): its data model "media_viewer" bound to a
// MediaViewerModel's page, the picture through the image source rv-texture:media-viewer-picture (the application
// registers the texture with UiSystem::setTexture) sized to fit the stage as laid out, the clicks on its buttons; since I5
// step 8 the clip through rv-texture:media-viewer-video, fitted likewise, and the manual's page.

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <rv/core/expected.hpp>
#include <rv/ui/media_viewer_model.hpp>

namespace rv::ui {

class UiView;

/// What a click on one of the viewer's buttons asks (I5 step 8: the clip's pause, "Apri il manuale").
enum class ViewerClick : std::uint8_t { Previous, Next, Close, Pause, Manual };

/// "previous", "next", "close", "pause", "manual" (the page event's argument).
[[nodiscard]] std::string_view toString(ViewerClick click) noexcept;
[[nodiscard]] std::optional<ViewerClick> viewerClickFromName(std::string_view name) noexcept;

/// The viewer's document in `view`'s context, over every other document while it shows. It never takes RmlUi's focus
/// (the application gives it the UI commands); pointer events reach it through the view. Main thread only; destroy it
/// before its view.
class MediaViewerScreen {
public:
    /// Registers the data model and loads the document, hidden. Errors: the data model exists in the context already
    /// (InvalidArgument), the document cannot be loaded (IoError, details in the log).
    [[nodiscard]] static Expected<std::unique_ptr<MediaViewerScreen>> create(UiView& view);
    ~MediaViewerScreen();
    MediaViewerScreen(const MediaViewerScreen&) = delete;
    MediaViewerScreen& operator=(const MediaViewerScreen&) = delete;

    /// Shows `model`'s page (bound only when something changed), brought to the front when it was hidden.
    void show(const MediaViewerModel& model);
    void hide();
    [[nodiscard]] bool visible() const noexcept;
    /// After UiView::update(): the stage as laid out; when its size changed, the picture is fitted again (bound now,
    /// laid out by the next update).
    void afterUpdate();

    /// The button a click pressed since the last call.
    [[nodiscard]] std::optional<ViewerClick> takeClicked() noexcept;
    /// The picture's box as laid out (x, y from the view's top-left corner, width, height, in pixels); nothing while
    /// it is hidden or shows no picture.
    [[nodiscard]] std::optional<glm::vec4> pictureBox() const;
    /// I5 step 8: the clip's box as laid out on the videos page, likewise (nothing until its first frame shows).
    [[nodiscard]] std::optional<glm::vec4> videoBox() const;
    /// The stage's box as laid out, likewise.
    [[nodiscard]] std::optional<glm::vec4> stageBox() const;

private:
    struct Impl;

    explicit MediaViewerScreen(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

} // namespace rv::ui
