#pragma once

// GPU-free (part of rv_app_cli): no SDL, OpenGL or FFmpeg header is included.
//
// What the hero CRT shows (I3 step 11, ADR-0012 §7; since I5 step 1 ADR-0016 §3 and §8): the game it plays (the start
// choice, then its playlist; a selection after its dwell, which interrupts the playlist and gives it back after its
// clip), the picture of that game (its video, or a still of the fallback chain: screenshot, title screen, cover, then
// its generated title card; the idle pattern only for no game at all), and the decoder's life around a launch (stopped
// before it, reopened at its position after the return, never again once Retroverse quits). The screen service of
// rv_app (screen_service.hpp) asks it every frame and does what it says with the one decoder, the still pictures and
// the material's texture override.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rv/app/playlist.hpp>
#include <rv/app/screen_game.hpp>

namespace rv::app {

/// How long an item must stay focused or inspected before the hero CRT plays its video (ADR-0012 §7), seconds of the
/// run's simulated time: scanning a shelf does not restart the decoder at every spine.
inline constexpr double kScreenDwellSeconds = 0.3;
/// How long a launch waits for the decoder's thread to end before the hand-over (ADR-0012 §6), seconds.
inline constexpr double kScreenStopBoundSeconds = 1.0;

/// What the hero CRT shows.
enum class ScreenPicture : std::uint8_t {
    IdlePattern, ///< the screen material's own picture (the test room's colour bars, the building's standby): no game
    Video,       ///< the game's clip
    Screenshot,  ///< the stills of the fallback chain, in its order (style §21, SC-08)
    TitleScreen,
    Cover,
    TitleCard, ///< the chain's end since I5 step 1 (ADR-0016 §8): a card generated from the game's title
};

/// "idle-pattern", "video", "screenshot", "title-screen", "cover", "title-card".
[[nodiscard]] std::string_view toString(ScreenPicture picture) noexcept;

/// The stills of `media` the chain shows, in its order (screenshot, title screen, cover), those it has (the title card,
/// which is no file, follows them: ScreenPolicy).
[[nodiscard]] std::vector<ScreenPicture> stillChain(const ScreenMedia& media);

/// The file of `picture` in `media`; null for the idle pattern and the title card, empty for a picture the game does
/// not have.
[[nodiscard]] const std::filesystem::path* mediaPath(const ScreenMedia& media, ScreenPicture picture) noexcept;

/// The game the hero CRT plays before anything is selected (ADR-0012 §7): the most recently played start candidate
/// with a video (isPlayableVideoFile), else the first one with one, in `games`' order (the featured order); nothing
/// when none has one (the idle pattern). Equal play times keep the earlier game.
[[nodiscard]] std::optional<std::size_t> chooseStartGame(std::span<const ScreenGame> games);

/// The game the hero CRT shows when no game has a playable video (I5 step 1, ADR-0016 §3: "with no clip in the
/// playlist the screen shows its game's still"): the first start candidate, in `games`' order, with a still or a title
/// card; nothing when none has either (the idle pattern).
[[nodiscard]] std::optional<std::size_t> chooseStillGame(std::span<const ScreenGame> games);

/// The decoder's life around launches (ADR-0012 §6).
enum class ScreenPhase : std::uint8_t {
    Running,  ///< the decoder may run
    Launched, ///< a launch started: the decoder is stopped until its end
    Quit,     ///< Retroverse quits: the decoder never starts again
};

/// "running", "launched", "quit".
[[nodiscard]] std::string_view toString(ScreenPhase phase) noexcept;

/// Why the screen plays its target (I5 step 1).
enum class ScreenOrigin : std::uint8_t {
    None,      ///< no game: the idle pattern
    Start,     ///< the start choice (chooseStartGame)
    Playlist,  ///< the playlist's game
    Selection, ///< the game focused, inspected or selected in the quick catalog, after its dwell
};

/// "none", "start", "playlist", "selection".
[[nodiscard]] std::string_view toString(ScreenOrigin origin) noexcept;

/// How the screen plays when nothing is selected (video.json's screen settings, ADR-0016 §3 and §4).
struct ScreenPlaySettings {
    /// The start choice, then the playlist ("enabled"); without, the screen plays selections only.
    bool autoplay = true;
    /// Selections play ("showOnFocus").
    bool showOnFocus = true;
    /// Clips are opened at all (video.json's global "enabled"); without, the stills and the card stand for them.
    bool clips = true;
    /// Seconds a game plays before the next (its clip's end when shorter): "previewDuration".
    double previewSeconds = 30.0;
    PlaylistOptions playlist;
    /// --video-freeze-pts: the start choice is held, nothing advances (ADR-0016 §3's frozen runs; selections still play).
    bool frozen = false;

    bool operator==(const ScreenPlaySettings&) const = default;
};

/// The hero CRT's decisions, frame by frame, without a GPU or a decoder (ADR-0012 §7, ADR-0016 §3):
///  - The target: the start choice (chooseStartGame, else chooseStillGame: a room without a clip shows its first game's
///    stills) for previewSeconds (its clip's end when shorter), then the
///    playlist's games one after the other (tick()), the game just shown skipped when the playlist has another. The item
///    focused or inspected (hover()) becomes the target once it has been so for kScreenDwellSeconds, held while it or
///    anything else is hovered; once nothing is (free roam, the quick catalog closed) its clip plays to its end, at most
///    previewSeconds, then the game the selection interrupted plays again from its start and the playlist goes on after
///    it. The item clocks run while the screen is in view and Running. An id the games do not list is a game without
///    media or card: the idle pattern. Each new target bumps targetGeneration().
///  - The picture of the target: its video once a frame of it is shown (videoShowing()), else the still of the fallback
///    chain that is ready (stillReady()), the title card last (for a game with a card, once its clip cannot play), else
///    the idle pattern;
///    wantedStill() is the still to load, the next one after stillFailed(). A video that is not playable
///    (isPlayableVideoFile) is never asked for; one whose decoder fails (videoFailed()) is not asked for again in the
///    run, and leaves the playlist.
///  - The phase: launchStarting() stops the decoder (wantsDecoder() false) and keeps the clip's position, which
///    takeResumePosition() hands to the decoder reopened after launchEnded() (the return, or a launch that failed or
///    was refused); quit() keeps it stopped for good.
///  - Visibility: setVisible(false) pauses the decoder (decoderPaused()), the picture kept.
class ScreenPolicy {
public:
    explicit ScreenPolicy(std::vector<ScreenGame> games = {}, ScreenPlaySettings settings = {});

    /// Replaces the games (the catalog changed): the start choice is made again while it plays; a target still listed
    /// keeps playing, its media updated; the playlist starts again over the new games.
    void setGames(std::vector<ScreenGame> games);
    /// Another screen with its games (I4 step 6b: the CRT of the room that holds the eye, rebound): nothing selected
    /// or hovered, the start choice made as at the start (a new target, its picture afresh); the phase kept, and the
    /// clips that failed.
    void restart(std::vector<ScreenGame> games);
    /// The same with that screen's settings (its room's, video.json).
    void restart(std::vector<ScreenGame> games, ScreenPlaySettings settings);
    [[nodiscard]] std::span<const ScreenGame> games() const noexcept { return m_games; }
    [[nodiscard]] const ScreenPlaySettings& settings() const noexcept { return m_settings; }
    [[nodiscard]] const Playlist& playlist() const noexcept { return m_playlist; }

    /// The item focused or inspected on this frame (its id), or nothing, at `nowSeconds` (the run's simulated time).
    void hover(std::optional<std::string_view> id, double nowSeconds);
    /// The frame's time passes (after hover()): the start choice, a playlist's game or a selection let go ends, and
    /// the next plays.
    void tick(double nowSeconds);

    /// What the screen plays: the target's id; empty for none (the idle pattern).
    [[nodiscard]] const std::string& targetId() const noexcept { return m_targetId; }
    /// The target's media (empty for none, or a game the list does not have).
    [[nodiscard]] const ScreenMedia& targetMedia() const noexcept { return m_targetMedia; }
    /// The target's title card (empty for none).
    [[nodiscard]] const ScreenCard& targetCard() const noexcept { return m_targetCard; }
    /// Why it plays.
    [[nodiscard]] ScreenOrigin origin() const noexcept { return m_origin; }
    /// Grows each time the target changes (0: the start choice).
    [[nodiscard]] std::uint64_t targetGeneration() const noexcept { return m_generation; }
    /// Selections that became the target after their dwell.
    [[nodiscard]] std::uint64_t selections() const noexcept { return m_selections; }
    /// The target came from a selection (not the start choice or the playlist).
    [[nodiscard]] bool selected() const noexcept { return m_selected; }
    /// Seconds the target has played (in view, Running).
    [[nodiscard]] double targetSeconds() const noexcept { return m_elapsed; }
    /// The selection's hold once let go (seconds left), nothing while hovered or without a selection.
    [[nodiscard]] std::optional<double> holdLeft() const noexcept { return m_holdLeft; }

    /// The picture shown now.
    [[nodiscard]] ScreenPicture picture() const noexcept;
    /// The target has a playable video that has not failed, and clips play: the screen opens it while the phase allows.
    [[nodiscard]] bool hasVideo() const noexcept;
    /// A decoder should run now: hasVideo() and the phase is Running.
    [[nodiscard]] bool wantsDecoder() const noexcept { return hasVideo() && m_phase == ScreenPhase::Running; }
    /// The still to load (the head of the chain not failed yet); nothing when none is left, and no title card while the
    /// target's clip may still play (hasVideo()).
    [[nodiscard]] std::optional<ScreenPicture> wantedStill() const noexcept;
    /// The still loaded and shown while the video is not (nothing yet).
    [[nodiscard]] std::optional<ScreenPicture> readyStill() const noexcept { return m_readyStill; }

    /// A frame of the target's video is on the screen.
    void videoShowing() noexcept { m_videoShowing = true; }
    /// The target's clip lasts `seconds` (known once its first frame shows): a shorter clip than previewSeconds plays
    /// to its end.
    void clipDuration(double seconds) noexcept;
    /// The target's video failed (its decoder's DecodeFailure, or it could not start): the stills from now on, and the
    /// clip skipped for the rest of the run.
    void videoFailed();
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
    void retarget(std::string id, ScreenOrigin origin);
    /// The start choice (autoplay), else no game.
    void startChoice();
    /// The playlist's next game (the one just shown skipped when there is another).
    void advance();
    /// The selection let go has played: the game it interrupted, else the playlist's next.
    void resume();
    /// What the target is given before the next: previewSeconds, its clip's duration when shorter.
    [[nodiscard]] double itemSeconds() const noexcept;
    /// `id` is listed with a playable clip that has not failed.
    [[nodiscard]] bool canPlay(std::string_view id) const;
    [[nodiscard]] const ScreenGame* find(std::string_view id) const noexcept;

    std::vector<ScreenGame> m_games;
    ScreenPlaySettings m_settings;
    Playlist m_playlist;
    /// Clips that failed in the run (by game id): never asked for again.
    std::set<std::string, std::less<>> m_failed;

    std::string m_targetId;
    ScreenMedia m_targetMedia;
    ScreenCard m_targetCard;
    ScreenOrigin m_origin = ScreenOrigin::None;
    std::uint64_t m_generation = 0;
    std::uint64_t m_selections = 0;
    bool m_selected = false;
    /// The game a selection interrupted (the start choice or the playlist's) and why it played.
    std::string m_interrupted;
    ScreenOrigin m_interruptedOrigin = ScreenOrigin::None;
    /// The hovered item and since when (simulated seconds).
    std::string m_hoverId;
    std::optional<double> m_hoverSince;
    /// The item's clock: the last tick, the seconds the target played, the clip's duration, the selection's hold left.
    std::optional<double> m_lastTick;
    double m_elapsed = 0.0;
    std::optional<double> m_clipSeconds;
    std::optional<double> m_holdLeft;

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
