#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// What the hero CRT shows (I3 step 11, ADR-0012 §7): the game it plays (the selection after its dwell, or the choice at
// the start), the picture of that game (its video, or a still of the fallback chain: screenshot, title screen, cover,
// then the idle pattern), and the decoder's life around a launch (stopped before it, reopened at its position after the
// return, never again once Retroverse quits). The screen service of rv_app (screen_service.hpp) asks it every frame and
// does what it says with the one decoder, the still pictures and the material's texture override.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rv::app {

/// How long an item must stay focused or inspected before the hero CRT plays its video (ADR-0012 §7), seconds of the
/// run's simulated time: scanning a shelf does not restart the decoder at every spine.
inline constexpr double kScreenDwellSeconds = 0.3;
/// How long a launch waits for the decoder's thread to end before the hand-over (ADR-0012 §6), seconds.
inline constexpr double kScreenStopBoundSeconds = 1.0;

/// What the hero CRT shows.
enum class ScreenPicture : std::uint8_t {
    IdlePattern, ///< the screen material's own picture (the colour bars): no game, or nothing of it could be shown
    Video,       ///< the game's clip
    Screenshot,  ///< the stills of the fallback chain, in its order (style §21, SC-08)
    TitleScreen,
    Cover,
};

/// "idle-pattern", "video", "screenshot", "title-screen", "cover".
[[nodiscard]] std::string_view toString(ScreenPicture picture) noexcept;

/// The media of a game the hero CRT can show: absolute paths, empty where the game has none.
struct ScreenMedia {
    std::filesystem::path video;
    std::filesystem::path screenshot;
    std::filesystem::path titleScreen;
    std::filesystem::path cover;

    bool operator==(const ScreenMedia&) const = default;
};

/// A game the hero CRT may play: its catalog id (ui::CatalogEntry::id, the GameId's text), its media and when it was
/// last played (any clock whose larger values are later, such as seconds since 1970; nothing: never).
struct ScreenGame {
    std::string id;
    ScreenMedia media;
    std::optional<std::int64_t> lastPlayed;

    bool operator==(const ScreenGame&) const = default;
};

/// The video extensions ES-DE reads (esde::kVideoExtensions, kept equal by a test: rv_app_cli does not link rv_esde).
inline constexpr std::string_view kScreenVideoExtensions[] = {".mp4", ".mkv", ".avi", ".wmv", ".mov", ".webm"};

/// True when `path` names a file ES-DE would play: one of kScreenVideoExtensions, in any case. Others (an `.flv`) are
/// never opened (ADR-0012 §7).
[[nodiscard]] bool isPlayableVideoFile(const std::filesystem::path& path);

/// The stills of `media` the chain shows, in its order (screenshot, title screen, cover), those it has.
[[nodiscard]] std::vector<ScreenPicture> stillChain(const ScreenMedia& media);

/// The file of `picture` in `media`; null for the idle pattern and for a picture the game does not have.
[[nodiscard]] const std::filesystem::path* mediaPath(const ScreenMedia& media, ScreenPicture picture) noexcept;

/// The game the hero CRT plays before anything is selected (ADR-0012 §7): the most recently played game with a video
/// (isPlayableVideoFile), else the first game with one, in `games`' order (the featured order); nothing when no game
/// has one (the idle pattern). Equal play times keep the earlier game.
[[nodiscard]] std::optional<std::size_t> chooseStartGame(std::span<const ScreenGame> games);

/// The decoder's life around launches (ADR-0012 §6).
enum class ScreenPhase : std::uint8_t {
    Running,  ///< the decoder may run
    Launched, ///< a launch started: the decoder is stopped until its end
    Quit,     ///< Retroverse quits: the decoder never starts again
};

/// "running", "launched", "quit".
[[nodiscard]] std::string_view toString(ScreenPhase phase) noexcept;

/// The hero CRT's decisions, frame by frame, without a GPU or a decoder (ADR-0012 §7):
///  - The target: the start choice (chooseStartGame) until the first selection; then the item focused or inspected
///    (hover()) once it has been so for kScreenDwellSeconds, kept while nothing else is (a hover that ends changes
///    nothing). An id the games do not list is a game without media: the idle pattern. Each new target bumps
///    targetGeneration().
///  - The picture of the target: its video once a frame of it is shown (videoShowing()), else the still of the fallback
///    chain that is ready (stillReady()), else the idle pattern; wantedStill() is the still to load, the next one after
///    stillFailed(). A video that is not playable (isPlayableVideoFile) is never asked for; one whose decoder fails
///    (videoFailed()) is not asked for again until the target changes.
///  - The phase: launchStarting() stops the decoder (wantsDecoder() false) and keeps the clip's position, which
///    takeResumePosition() hands to the decoder reopened after launchEnded() (the return, or a launch that failed or
///    was refused); quit() keeps it stopped for good.
///  - Visibility: setVisible(false) pauses the decoder (decoderPaused()), the picture kept.
class ScreenPolicy {
public:
    explicit ScreenPolicy(std::vector<ScreenGame> games = {});

    /// Replaces the games (the catalog changed): the start choice is made again while nothing was selected; a target
    /// still listed keeps playing, its media updated.
    void setGames(std::vector<ScreenGame> games);
    [[nodiscard]] std::span<const ScreenGame> games() const noexcept { return m_games; }

    /// The item focused or inspected on this frame (its id), or nothing, at `nowSeconds` (the run's simulated time).
    void hover(std::optional<std::string_view> id, double nowSeconds);

    /// What the screen plays: the target's id; empty for none (the idle pattern).
    [[nodiscard]] const std::string& targetId() const noexcept { return m_targetId; }
    /// The target's media (empty for none, or a game the list does not have).
    [[nodiscard]] const ScreenMedia& targetMedia() const noexcept { return m_targetMedia; }
    /// Grows each time the target changes (0: the start choice).
    [[nodiscard]] std::uint64_t targetGeneration() const noexcept { return m_generation; }
    /// Selections that became the target after their dwell.
    [[nodiscard]] std::uint64_t selections() const noexcept { return m_selections; }
    /// The target came from a selection (not the start choice).
    [[nodiscard]] bool selected() const noexcept { return m_selected; }

    /// The picture shown now.
    [[nodiscard]] ScreenPicture picture() const noexcept;
    /// The target has a playable video that has not failed: the screen opens it while the phase allows.
    [[nodiscard]] bool hasVideo() const noexcept;
    /// A decoder should run now: hasVideo() and the phase is Running.
    [[nodiscard]] bool wantsDecoder() const noexcept { return hasVideo() && m_phase == ScreenPhase::Running; }
    /// The still to load (the head of the chain not failed yet); nothing when none is left.
    [[nodiscard]] std::optional<ScreenPicture> wantedStill() const noexcept;
    /// The still loaded and shown while the video is not (nothing yet).
    [[nodiscard]] std::optional<ScreenPicture> readyStill() const noexcept { return m_readyStill; }

    /// A frame of the target's video is on the screen.
    void videoShowing() noexcept { m_videoShowing = true; }
    /// The target's video failed (its decoder's DecodeFailure, or it could not start): the stills from now on.
    void videoFailed() noexcept;
    /// wantedStill() is loaded and shown while the video is not.
    void stillReady() noexcept;
    /// wantedStill() cannot be shown (missing, not an image, damaged): the next one is wanted.
    void stillFailed() noexcept;

    /// A launch starts: the decoder stops, `clipPosition` (seconds into the clip shown, if a video showed) is kept for
    /// the return. Nothing once quit.
    void launchStarting(std::optional<double> clipPosition) noexcept;
    /// The launch ended (the return), failed to start or was refused: the decoder may run again, at the kept position.
    void launchEnded() noexcept;
    /// Retroverse quits: the decoder stays stopped.
    void quit() noexcept { m_phase = ScreenPhase::Quit; }
    [[nodiscard]] ScreenPhase phase() const noexcept { return m_phase; }
    /// The position the reopened decoder seeks to, once (after launchEnded()); nothing otherwise.
    [[nodiscard]] std::optional<double> takeResumePosition() noexcept;

    /// The hero CRT is in the view (a decoder of a screen out of view pauses: ADR-0012 §7).
    void setVisible(bool visible) noexcept { m_visible = visible; }
    [[nodiscard]] bool visible() const noexcept { return m_visible; }
    [[nodiscard]] bool decoderPaused() const noexcept { return !m_visible; }

private:
    /// Makes `id` the target (its media from the list, none when unlisted) and starts its picture afresh.
    void retarget(std::string id);
    [[nodiscard]] const ScreenGame* find(std::string_view id) const noexcept;

    std::vector<ScreenGame> m_games;
    std::string m_targetId;
    ScreenMedia m_targetMedia;
    std::uint64_t m_generation = 0;
    std::uint64_t m_selections = 0;
    bool m_selected = false;
    /// The hovered item and since when (simulated seconds).
    std::string m_hoverId;
    std::optional<double> m_hoverSince;

    std::vector<ScreenPicture> m_chain;
    std::size_t m_chainIndex = 0;
    std::optional<ScreenPicture> m_readyStill;
    bool m_videoShowing = false;
    bool m_videoFailed = false;

    ScreenPhase m_phase = ScreenPhase::Running;
    std::optional<double> m_launchPosition;
    std::optional<double> m_resumePosition;
    bool m_visible = true;
};

} // namespace rv::app
