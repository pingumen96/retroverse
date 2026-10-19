#pragma once

// GPU-free (part of rv_ui_model): no SDL, OpenGL or RmlUi header is included.
//
// The media viewer (I3 step 14b; ADR-0012 §5, specification §12.1 "Vedi media"): the images a game has, one page per
// image kind, in the order media_viewer::kKinds gives (cover, back cover, physical media, 3D box, screenshot, title
// screen, marquee, fan art), the kinds it lacks skipped. Left and right (the D-pad, the arrows, the shoulders, the
// triggers) page through them without wrapping, Home and End jump to the first and the last, Back closes the viewer.
// Each page's picture is the application's texture (rv-texture:media-viewer-picture), read by the application from the
// game's media files (RmlUi never loads the user's files): until it comes the page says it is loading, a file that
// cannot be read or decoded shows the page's fallback. Every sentence is the document's (assets/ui/media_viewer.rml):
// the code passes kinds and states. Videos and manuals in the viewer are I5's.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/ui/ui_command.hpp>

namespace rv::ui {

/// The ids, the data model and the texture key the viewer's document (assets/ui/media_viewer.rml) and its screen
/// (rv::ui::MediaViewerScreen) share (tests/unit/ui/test_media_viewer.cpp checks the document against them).
namespace media_viewer {
inline constexpr std::string_view kDocumentPath = "ui/media_viewer.rml";
inline constexpr std::string_view kStyleSheetPath = "ui/media_viewer.rcss";
inline constexpr std::string_view kDataModel = "media_viewer";
inline constexpr std::string_view kViewerId = "media-viewer";
inline constexpr std::string_view kTitleId = "media-viewer-title";
/// The area the picture is fitted into (its box as laid out decides the picture's size), and the picture: an <img>
/// whose source is kPictureSource, the application's texture registered under kPictureTexture.
inline constexpr std::string_view kStageId = "media-viewer-stage";
inline constexpr std::string_view kPictureId = "media-viewer-picture";
inline constexpr std::string_view kPictureTexture = "media-viewer-picture";
inline constexpr std::string_view kPictureSource = "rv-texture:media-viewer-picture";
/// The buttons (a click pages or closes) and the data event they send: "page('<previous|next|close>')".
inline constexpr std::string_view kPreviousId = "media-previous";
inline constexpr std::string_view kNextId = "media-next";
inline constexpr std::string_view kCloseId = "media-close";
inline constexpr std::string_view kPageEvent = "page";
/// The image kinds the viewer pages, in its order (ADR-0012 §5), named as ES-DE names their folders
/// (catalog::toString(MediaKind), which rv_ui does not link): the document labels each.
inline constexpr std::array<std::string_view, 8> kKinds{"covers",       "backcovers",   "physicalmedia",
                                                        "3dboxes",      "screenshots",  "titlescreens",
                                                        "marquees",     "fanart"};
/// The most a picture is enlarged to fill the stage (a 16 px picture stays small).
inline constexpr float kMaxEnlargement = 4.0f;
} // namespace media_viewer

/// Where a page's picture stands.
enum class PictureState : std::uint8_t {
    Loading, ///< asked for, not there yet
    Shown,   ///< its texture is registered
    Failed,  ///< its file cannot be read or decoded: the page's fallback
};

/// "loading", "shown", "failed".
[[nodiscard]] std::string_view toString(PictureState state) noexcept;

/// One page of the viewer: an image kind of the game.
struct MediaPage {
    /// One of media_viewer::kKinds.
    std::string kind;
    PictureState state = PictureState::Loading;
    /// The picture's pixels once shown (its aspect sizes it in the stage).
    int width = 0;
    int height = 0;

    bool operator==(const MediaPage&) const = default;
};

/// What a command did to the viewer.
enum class ViewerStep : std::uint8_t {
    None,  ///< nothing (an edge, a command the viewer does not take)
    Moved, ///< another page shows
    Close, ///< Back: the viewer closes
};

/// The viewer's pages and the page shown. Main thread; no rendering.
class MediaViewerModel {
public:
    /// Opens on `gameId` (its title and platform shown as they come) with the image kinds it has, in any order: one
    /// page per kind of media_viewer::kKinds that `kinds` names, in kKinds' order (other kinds and repeats ignored),
    /// the first shown. A game without any shows the document's "no image" sentence.
    void open(std::string gameId, std::string title, std::string platform, std::span<const std::string> kinds);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return m_open; }

    /// Left, PageUp and FilterPrevious show the previous page, Right, PageDown, FilterNext and Activate the next one
    /// (no wrap), Home the first, End the last; Back closes. Other commands: None.
    ViewerStep apply(UiCommand command) noexcept;
    /// Shows page `index` (Moved), None when it is the page shown or out of range.
    ViewerStep select(std::size_t index) noexcept;

    /// The page's picture came (Shown, its size) or failed; nothing for an index out of range.
    void setPicture(std::size_t index, PictureState state, int width = 0, int height = 0) noexcept;

    [[nodiscard]] const std::string& gameId() const noexcept { return m_gameId; }
    [[nodiscard]] const std::string& title() const noexcept { return m_title; }
    [[nodiscard]] const std::string& platform() const noexcept { return m_platform; }
    [[nodiscard]] const std::vector<MediaPage>& pages() const noexcept { return m_pages; }
    [[nodiscard]] std::size_t index() const noexcept { return m_index; }
    /// The page shown; null without pages.
    [[nodiscard]] const MediaPage* current() const noexcept;
    /// The index of `kind`'s page, if the game has it.
    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view kind) const noexcept;

private:
    bool m_open = false;
    std::string m_gameId;
    std::string m_title;
    std::string m_platform;
    std::vector<MediaPage> m_pages;
    std::size_t m_index = 0;
};

/// The picture's size in the stage (`stageWidth` x `stageHeight`, any unit): the largest that fits without stretching,
/// enlarged at most media_viewer::kMaxEnlargement times; 0 x 0 for an empty stage or picture.
struct PictureFit {
    float width = 0.0f;
    float height = 0.0f;

    bool operator==(const PictureFit&) const = default;
};
[[nodiscard]] PictureFit fitPicture(float stageWidth, float stageHeight, int pictureWidth, int pictureHeight) noexcept;

} // namespace rv::ui
