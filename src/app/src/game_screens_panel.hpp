#pragma once

// The media viewer and Informazioni of the product UI (I3 step 14b; ADR-0012 §5): ui::MediaViewerScreen and
// ui::GameInfoScreen over every other document of the view, opened from the world's card (its Vedi media and
// Informazioni, the pad's Y and X) and from the quick catalog's own buttons alike, one at a time (Vedi media from
// Informazioni, or Informazioni from the viewer, replaces it). The viewer's pictures are read on a loader thread of
// its own (the media root as ES-DE's settings give it now, each page's file with core::readFileShared, decoded and
// reduced: app::viewerPicture), every page of the game asked at once, the page shown first, and uploaded on the main
// thread as RGBA8 textures holding the sRGB values as encoded (the UI composites sRGB values), at most two a frame;
// the page shown is registered for the document as rv-texture:media-viewer-picture. RmlUi never loads the user's
// files, and nothing is ever written under the media root. The textures go when the viewer closes. Main thread only;
// destroy it while the OpenGL context is current.
//
// I5 step 8 (ADR-0016 §10): the loader also finds the clip's file (its path below the media root) and the manual's
// (there or not). The videos page asks the screens' service for its clip (viewerRequest(): the application hands it to
// ScreenService::setViewer and gives back ScreenService::viewerVideo() through setViewerVideo(), which registers the
// view of the clip's texture as rv-texture:media-viewer-video while the page shows); Activate pauses it and plays it
// again; a clip that cannot play shows the page's fallback with its reason. The manuals page's "Apri il manuale" opens
// the PDF in Windows' default program: launcher::planManualOpen (explorer.exe and the file as its one argument) started
// through a Win32ProcessRunner of its own (no shell; the automation guard refuses it in automated runs), not followed,
// no game session: Retroverse keeps running.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/game_screens.hpp>
#include <rv/app/run_report.hpp>
#include <rv/core/expected.hpp>
#include <rv/ui/game_info_model.hpp>
#include <rv/ui/ui_command.hpp>

namespace rv::ui {
class UiSystem;
class UiView;
} // namespace rv::ui

namespace rv::app {

class GameScreensPanel {
public:
    /// The viewer of one game.
    struct MediaRequest {
        std::string gameId;
        std::string title;
        std::string platform;
        /// kScreenFromCard or kScreenFromCatalog.
        std::string from;
        std::vector<MediaPageFile> pages;
    };

    /// What a command or a click asks of the product UI, which has the catalog: the other screen of the same game.
    /// I7 step 6: or what a row of Informazioni asks: EmulatorList, VersionLaunch (asked()'s version), the status stepped.
    enum class Switch : std::uint8_t { None, Media, Info, EmulatorList, VersionLaunch, StatusNext, StatusPrevious };

    /// Creates the two screens (hidden) in `view` and the loader thread. `libraryFile` is library.json (none: the mock
    /// catalog, whose games have no media); `wake` (thread-safe) is called by the loader when a picture is ready.
    /// Errors: a document cannot be loaded.
    [[nodiscard]] static Expected<std::unique_ptr<GameScreensPanel>>
    create(ui::UiView& view, ui::UiSystem& system, std::optional<std::filesystem::path> libraryFile,
           std::function<void()> wake);
    /// Stops and joins the loader (its picture in flight dropped) and deletes the textures.
    ~GameScreensPanel();
    GameScreensPanel(const GameScreensPanel&) = delete;
    GameScreensPanel& operator=(const GameScreensPanel&) = delete;

    /// Opens the viewer on `request`'s game (Informazioni closed), its first page shown; every page's picture asked.
    void openMedia(MediaRequest request, std::uint64_t frame);
    /// Opens Informazioni on `content` (the viewer closed).
    void openInfo(ui::GameInfoContent content, std::string from, std::uint64_t frame);
    /// Back: the screen shown closes.
    void close(std::uint64_t frame);

    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool viewerOpen() const noexcept;
    [[nodiscard]] bool infoOpen() const noexcept;
    /// The game the screen shown is about (its GameId's text), and where it was opened from.
    [[nodiscard]] const std::string& gameId() const noexcept { return m_gameId; }
    [[nodiscard]] const std::string& from() const noexcept { return m_from; }

    /// A UI command while a screen shows (Back aside: the application's levels call close()): the viewer pages, the
    /// body of Informazioni scrolls; Info on the viewer and Media on Informazioni ask for the other screen. Since I5
    /// step 8 Activate on the videos page pauses its clip or plays it again, on the manuals page opens the manual.
    [[nodiscard]] Switch apply(ui::UiCommand command, std::uint64_t frame);
    /// Once a frame, before the view's update: the clicks (a page, the other screen, close, the clip's pause, the
    /// manual), the pictures and files the loader brought (at most two pictures uploaded), the screen shown bound.
    [[nodiscard]] Switch update(std::uint64_t frame);
    /// After the view's update: the viewer fits its picture to the stage as laid out; the record's boxes.
    void afterUpdate();
    /// Pictures are being read (lockstep runs wait for them, as for the catalog worker).
    [[nodiscard]] bool loading() const noexcept;

    /// I5 step 8: what the viewer asks of the screens' service on this frame (open; its videos page's clip once its
    /// file is found, while that page shows and its clip has not failed).
    [[nodiscard]] ViewerRequest viewerRequest() const;
    /// I5 step 8: the clip as the service left it after its frame: the page's state, the clip's texture registered as
    /// rv-texture:media-viewer-video while the videos page shows (emptied otherwise, and before the viewer closes).
    void setViewerVideo(const ViewerVideo& video, std::uint64_t frame);

    /// I7 step 6 (ADR-0018 §11): Informazioni's content while it shows, and its focused row (nothing: the text).
    [[nodiscard]] const ui::GameInfoContent* infoContent() const noexcept;
    [[nodiscard]] std::optional<ui::InfoRow> infoRow() const noexcept;
    /// Its rows as the catalog worker answered.
    void setInfoControls(ui::GameInfoControls controls, std::uint64_t frame);
    /// A step the product UI took for a row, for the run report.
    void noteInfo(std::uint64_t frame, std::string event, std::string detail);

    [[nodiscard]] const GameScreensRecord& record() const noexcept { return m_record; }

private:
    struct Impl;

    GameScreensPanel(std::unique_ptr<Impl> impl) noexcept;

    void addEvent(std::uint64_t frame, std::string event, std::string kind = {}, std::string detail = {});
    void showPage(std::uint64_t frame);
    /// I5 step 8: "Apri il manuale" on the manuals page.
    void openManual(std::uint64_t frame);
    /// I5 step 8: the clip's pause toggled on the videos page (the model flipped it).
    void videoPauseToggled(std::uint64_t frame);
    /// I7 step 6: the focused row of Informazioni, and what a row pressed asks.
    void setInfoRow(std::optional<ui::InfoRow> row, std::uint64_t frame);
    [[nodiscard]] Switch pressInfoRow(const ui::InfoRow& row, std::uint64_t frame);

    std::unique_ptr<Impl> m_impl;
    std::string m_gameId;
    std::string m_from;
    GameScreensRecord m_record;
};

} // namespace rv::app
