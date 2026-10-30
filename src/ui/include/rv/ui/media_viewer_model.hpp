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
// the code passes kinds and states.
//
// I5 step 8 (ADR-0016 §10): the `videos` page after the title screen plays the game's clip with its sound
// (rv-texture:media-viewer-video, the application's view of the decoder slot's texture), Activate pausing it and
// playing it again, a clip that cannot play showing the page's fallback with its reason (a video verdict's name, or the
// decoder's failure); the `manuals` page, the last, names the manual and opens it in Windows' default program with
// Activate ("Apri il manuale"), its outcome shown (opened, refused by the automation guard, failed).

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
/// I5 step 8: the clip of the videos page, an <img> whose source is kVideoSource (the application's texture registered
/// under kVideoTexture: a GL_RGBA8 view of the decoder slot's sRGB texture), and the buttons of the clip and the manual
/// (their clicks send "page('pause')" and "page('manual')").
inline constexpr std::string_view kVideoId = "media-viewer-video";
inline constexpr std::string_view kVideoTexture = "media-viewer-video";
inline constexpr std::string_view kVideoSource = "rv-texture:media-viewer-video";
inline constexpr std::string_view kPauseButtonId = "media-video-pause";
inline constexpr std::string_view kManualButtonId = "media-manual-open";
/// The buttons (a click pages or closes) and the data event they send: "page('<previous|next|close>')".
inline constexpr std::string_view kPreviousId = "media-previous";
inline constexpr std::string_view kNextId = "media-next";
inline constexpr std::string_view kCloseId = "media-close";
inline constexpr std::string_view kPageEvent = "page";
/// The kinds the viewer pages, in its order (ADR-0012 §5; since I5 step 8, ADR-0016 §10, the clip after the title
/// screen and the manual last), named as ES-DE names their folders (catalog::toString(MediaKind), which rv_ui does not
/// link): the document labels each.
inline constexpr std::string_view kVideosKind = "videos";
inline constexpr std::string_view kManualsKind = "manuals";
inline constexpr std::array<std::string_view, 10> kKinds{"covers",      "backcovers",   "physicalmedia", "3dboxes",
                                                         "screenshots", "titlescreens", kVideosKind,     "marquees",
                                                         "fanart",      kManualsKind};
/// The reasons a clip cannot play that the document has a sentence for (a video verdict's name, media::verdictName(),
/// or the screens' service's: "not-playable" for an extension ES-DE does not play, "graphics-error" for a texture that
/// could not be made); any other shows the generic sentence with the reason.
inline constexpr std::array<std::string_view, 10> kVideoReasons{
    "file-not-found", "invalid-file",  "no-video-stream", "unsupported-codec", "corrupt-data",
    "truncated",      "decoder-error", "read-error",      "not-playable",      "graphics-error"};
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

/// What "Apri il manuale" did (I5 step 8).
enum class ManualState : std::uint8_t {
    Ready,   ///< not asked yet
    Opened,  ///< explorer.exe started with the manual: Windows' default program opens it
    Refused, ///< the automation guard refused it (RV_LAUNCH_ALLOWLIST: an automated run never opens a window)
    Failed,  ///< it could not be started (a path explorer.exe cannot read, the program missing, ...)
};

/// "ready", "opened", "refused", "failed".
[[nodiscard]] std::string_view toString(ManualState state) noexcept;

/// True for the kinds whose page shows a picture (not the clip, not the manual).
[[nodiscard]] bool isPictureKind(std::string_view kind) noexcept;

/// One page of the viewer: a media kind of the game.
struct MediaPage {
    /// One of media_viewer::kKinds.
    std::string kind;
    /// A picture: its texture's state. The clip (I5 step 8): Loading until its first frame shows, Shown while it plays
    /// or pauses, Failed when it cannot play (`reason`). The manual: Loading while its file is looked for, Shown once
    /// found, Failed when it is not there (`reason`).
    PictureState state = PictureState::Loading;
    /// The picture's (or the clip's) pixels once shown (its aspect sizes it in the stage).
    int width = 0;
    int height = 0;
    /// I5 step 8: the clip paused (Activate); why the clip cannot play or the manual is missing or did not open
    /// (media_viewer::kVideoReasons, or the outcome's reason); the manual's file name; what "Apri il manuale" did.
    bool paused = false;
    std::string reason;
    std::string name;
    ManualState manual = ManualState::Ready;

    bool operator==(const MediaPage&) const = default;
};

/// What a command did to the viewer.
enum class ViewerStep : std::uint8_t {
    None,  ///< nothing (an edge, a command the viewer does not take)
    Moved, ///< another page shows
    Close, ///< Back: the viewer closes
    /// I5 step 8: Activate on the videos page (its clip showing): the clip pauses, or plays again.
    TogglePause,
    /// I5 step 8: Activate on the manuals page (its file found): "Apri il manuale".
    OpenManual,
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
    /// (no wrap), Home the first, End the last; Back closes. Since I5 step 8 Activate on the videos page toggles its
    /// pause (TogglePause, the page's `paused` flipped; None until its clip shows), on the manuals page asks for the
    /// manual (OpenManual once its file is found, None before). Other commands: None.
    ViewerStep apply(UiCommand command) noexcept;
    /// Shows page `index` (Moved; the page left loses its pause), None when it is the page shown or out of range.
    ViewerStep select(std::size_t index) noexcept;

    /// The page's picture came (Shown, its size) or failed; nothing for an index out of range.
    void setPicture(std::size_t index, PictureState state, int width = 0, int height = 0) noexcept;
    /// I5 step 8: the clip's state on its page (Shown with its frame's size, Failed with `reason`); the pause is the
    /// page's own (TogglePause). Nothing for an index out of range or a page of another kind.
    void setVideo(std::size_t index, PictureState state, int width = 0, int height = 0, std::string reason = {});
    /// I5 step 8: the manual's file was looked for (Shown: there, its file name `name`; Failed with `reason`).
    void setManualFile(std::size_t index, PictureState state, std::string name, std::string reason = {});
    /// I5 step 8: what "Apri il manuale" did (`reason`: why it was refused or failed).
    void setManualOutcome(std::size_t index, ManualState outcome, std::string reason = {});

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
