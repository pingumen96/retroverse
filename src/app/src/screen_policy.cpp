#include <rv/app/screen_policy.hpp>
#include <rv/core/utf8.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace rv::app {
namespace {

/// Seconds a dwell may fall short by and still count: a fixed step of 1/60 s adds up to 0.3 s in 18 frames only up to
/// rounding.
constexpr double kDwellTolerance = 1e-6;

/// ASCII lower case (extensions are ASCII; anything else never matches).
char lowerAscii(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::string_view toString(ScreenPicture picture) noexcept {
    switch (picture) {
    case ScreenPicture::IdlePattern:
        return "idle-pattern";
    case ScreenPicture::Video:
        return "video";
    case ScreenPicture::Screenshot:
        return "screenshot";
    case ScreenPicture::TitleScreen:
        return "title-screen";
    case ScreenPicture::Cover:
        return "cover";
    case ScreenPicture::TitleCard:
        return "title-card";
    }
    return "unknown";
}

std::string_view toString(ScreenOrigin origin) noexcept {
    switch (origin) {
    case ScreenOrigin::None:
        return "none";
    case ScreenOrigin::Start:
        return "start";
    case ScreenOrigin::Playlist:
        return "playlist";
    case ScreenOrigin::Selection:
        return "selection";
    }
    return "unknown";
}

std::string_view toString(ScreenPhase phase) noexcept {
    switch (phase) {
    case ScreenPhase::Running:
        return "running";
    case ScreenPhase::Launched:
        return "launched";
    case ScreenPhase::Quit:
        return "quit";
    }
    return "unknown";
}

bool isPlayableVideoFile(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    const Expected<std::string> extension = core::tryPathToUtf8(path.extension());
    if (!extension) {
        return false;
    }
    return std::any_of(
        std::begin(kScreenVideoExtensions), std::end(kScreenVideoExtensions), [&extension](std::string_view known) {
            return extension->size() == known.size() && std::equal(known.begin(), known.end(), extension->begin(),
                                                                   [](char a, char b) { return a == lowerAscii(b); });
        });
}

std::vector<ScreenPicture> stillChain(const ScreenMedia& media) {
    std::vector<ScreenPicture> chain;
    for (const ScreenPicture picture : {ScreenPicture::Screenshot, ScreenPicture::TitleScreen, ScreenPicture::Cover}) {
        if (!mediaPath(media, picture)->empty()) {
            chain.push_back(picture);
        }
    }
    return chain;
}

const std::filesystem::path* mediaPath(const ScreenMedia& media, ScreenPicture picture) noexcept {
    switch (picture) {
    case ScreenPicture::Video:
        return &media.video;
    case ScreenPicture::Screenshot:
        return &media.screenshot;
    case ScreenPicture::TitleScreen:
        return &media.titleScreen;
    case ScreenPicture::Cover:
        return &media.cover;
    case ScreenPicture::IdlePattern:
    case ScreenPicture::TitleCard:
        break;
    }
    return nullptr;
}

std::optional<std::size_t> chooseStartGame(std::span<const ScreenGame> games) {
    std::optional<std::size_t> played;
    std::optional<std::size_t> first;
    for (std::size_t index = 0; index < games.size(); ++index) {
        const ScreenGame& game = games[index];
        if (!game.startCandidate || !isPlayableVideoFile(game.media.video)) {
            continue;
        }
        if (!first) {
            first = index;
        }
        if (game.lastPlayed && (!played || *game.lastPlayed > *games[*played].lastPlayed)) {
            played = index;
        }
    }
    return played ? played : first;
}

std::optional<std::size_t> chooseStillGame(std::span<const ScreenGame> games) {
    for (std::size_t index = 0; index < games.size(); ++index) {
        const ScreenGame& game = games[index];
        if (game.startCandidate && (!stillChain(game.media).empty() || !game.card.empty())) {
            return index;
        }
    }
    return std::nullopt;
}

ScreenPolicy::ScreenPolicy(std::vector<ScreenGame> games, ScreenPlaySettings settings)
    : m_games(std::move(games)), m_settings(std::move(settings)) {
    m_playlist = Playlist(m_games, m_settings.playlist, m_failed);
    startChoice();
    m_generation = 0; // the start choice is the first target
}

std::optional<std::size_t> ScreenPolicy::startIndex() const {
    if (!m_settings.autoplay) {
        return std::nullopt;
    }
    const std::optional<std::size_t> start = chooseStartGame(m_games);
    return start ? start : chooseStillGame(m_games); // no clip in the room: its first game's stills, its card last
}

void ScreenPolicy::startChoice() {
    const std::optional<std::size_t> start = startIndex();
    if (start) {
        retarget(m_games[*start].id, ScreenOrigin::Start);
    } else {
        retarget(std::string(), ScreenOrigin::None);
    }
}

void ScreenPolicy::setGames(std::vector<ScreenGame> games) {
    m_games = std::move(games);
    m_playlist = Playlist(m_games, m_settings.playlist, m_failed);
    if (!m_selected && m_origin != ScreenOrigin::Playlist) {
        const std::optional<std::size_t> start = startIndex();
        std::string id = start ? m_games[*start].id : std::string();
        if (id != m_targetId) {
            retarget(std::move(id), start ? ScreenOrigin::Start : ScreenOrigin::None);
            return;
        }
    }
    if (!m_selected && m_origin == ScreenOrigin::Playlist && !canPlay(m_targetId)) {
        advance(); // the playlist's game left the room, or lost its clip
        return;
    }
    const ScreenGame* listed = find(m_targetId);
    const ScreenMedia media = listed != nullptr ? listed->media : ScreenMedia{};
    const ScreenCard card = listed != nullptr ? listed->card : ScreenCard{};
    if (!m_targetId.empty() && (media != m_targetMedia || card != m_targetCard)) {
        retarget(m_targetId, m_origin); // its media changed: shown afresh
    }
}

void ScreenPolicy::restart(std::vector<ScreenGame> games) {
    restart(std::move(games), m_settings);
}

void ScreenPolicy::restart(std::vector<ScreenGame> games, ScreenPlaySettings settings) {
    m_games = std::move(games);
    m_settings = std::move(settings);
    m_playlist = Playlist(m_games, m_settings.playlist, m_failed);
    m_selected = false;
    m_interrupted.clear();
    m_interruptedOrigin = ScreenOrigin::None;
    m_hoverId.clear();
    m_hoverSince.reset();
    startChoice();
}

void ScreenPolicy::hover(std::optional<std::string_view> id, double nowSeconds) {
    if (!id || id->empty() || !m_settings.showOnFocus) {
        m_hoverId.clear();
        m_hoverSince.reset();
        return;
    }
    if (*id != m_hoverId || !m_hoverSince) {
        m_hoverId = std::string(*id);
        m_hoverSince = nowSeconds;
    }
    if (m_selected && m_hoverId == m_targetId) {
        return;
    }
    if (nowSeconds - *m_hoverSince + kDwellTolerance < kScreenDwellSeconds) {
        return;
    }
    ++m_selections;
    if (!m_selected && m_interrupted.empty()) {
        // The game the selection interrupts plays again once the selection has played (ADR-0016 §3).
        m_interrupted = m_targetId;
        m_interruptedOrigin = m_origin;
    }
    m_selected = true;
    if (m_hoverId != m_targetId) {
        retarget(m_hoverId, ScreenOrigin::Selection);
    } else {
        m_origin = ScreenOrigin::Selection;
        m_holdLeft.reset();
    }
}

void ScreenPolicy::tick(double nowSeconds) {
    const double step = m_lastTick ? std::max(0.0, nowSeconds - *m_lastTick) : 0.0;
    m_lastTick = nowSeconds;
    if (m_settings.frozen || m_phase != ScreenPhase::Running || m_targetId.empty()) {
        return;
    }
    // The item's clock runs while its screen plays: in view, no game running.
    const double played = m_visible ? step : 0.0;
    m_elapsed += played;
    if (m_selected) {
        if (!m_hoverId.empty()) {
            m_holdLeft.reset(); // still focused, inspected or selected: held
            return;
        }
        if (!m_holdLeft) {
            // Let go: its clip plays to its end (the current pass of the looping clip), at most previewSeconds.
            double hold = m_settings.previewSeconds;
            if (m_videoShowing && m_clipSeconds && *m_clipSeconds > 0.0) {
                hold = std::min(hold, *m_clipSeconds - clipPhase());
            }
            m_holdLeft = hold;
            return;
        }
        *m_holdLeft -= played;
        if (*m_holdLeft <= kDwellTolerance) {
            resume();
        }
        return;
    }
    if (!m_settings.autoplay) {
        return;
    }
    if (itemPlayed()) {
        advance();
    }
}

bool ScreenPolicy::itemPlayed() const noexcept {
    if (m_videoShowing && m_clipShownAt && m_clipSeconds && *m_clipSeconds > 0.0 &&
        *m_clipSeconds < m_settings.previewSeconds) {
        // A clip shorter than previewSeconds plays to its end, counted from its first frame: the frames the decoder
        // took to deliver it are not taken from it.
        return clipSeconds() + kDwellTolerance >= *m_clipSeconds;
    }
    return m_elapsed + kDwellTolerance >= m_settings.previewSeconds;
}

double ScreenPolicy::clipSeconds() const noexcept {
    return m_clipShownAt ? m_elapsed - *m_clipShownAt : m_elapsed;
}

double ScreenPolicy::clipPhase() const noexcept {
    const double position = clipSeconds();
    if (!m_clipSeconds || *m_clipSeconds <= 0.0) {
        return position;
    }
    // Whole passes taken off; a pass that ended within the tolerance counts as ended (its phase 0, not the clip's
    // length).
    return position - *m_clipSeconds * std::floor((position + kDwellTolerance) / *m_clipSeconds);
}

void ScreenPolicy::restartClock() noexcept {
    const double phase = clipPhase();
    m_elapsed = 0.0;
    if (m_clipShownAt) {
        m_clipShownAt = -phase; // the looping clip goes on: its passes keep their phase
    }
}

bool ScreenPolicy::advance() {
    std::optional<std::string> next = m_playlist.next();
    if (next && *next == m_targetId) {
        next = m_playlist.next(); // never the game just shown, when the playlist has another
    }
    if (!next || *next == m_targetId) {
        // One game, or none left: the target keeps playing (its clip loops), its time counted afresh.
        restartClock();
        if (next) {
            m_origin = ScreenOrigin::Playlist;
        }
        return next.has_value();
    }
    retarget(std::move(*next), ScreenOrigin::Playlist);
    return true;
}

void ScreenPolicy::resume() {
    m_selected = false;
    m_holdLeft.reset();
    std::string back = std::exchange(m_interrupted, std::string());
    const ScreenOrigin backOrigin = std::exchange(m_interruptedOrigin, ScreenOrigin::None);
    if (!m_settings.autoplay) {
        retarget(std::string(), ScreenOrigin::None); // selections only: back to no game
        return;
    }
    // The game interrupted, while the room still has it: its clip, or its stills and card when the clip failed or
    // clips are off.
    if (!back.empty() && back != m_targetId && find(back) != nullptr) {
        retarget(std::move(back), backOrigin);
        return;
    }
    if (advance()) {
        return;
    }
    // No playlist to go on with (a room without a clip, or every clip failed): the start choice, never the selection
    // held for good.
    const std::optional<std::size_t> start = startIndex();
    if (start && m_games[*start].id == m_targetId) {
        m_origin = ScreenOrigin::Start; // the selection was the start choice: it plays on, not restarted
    } else if (start) {
        retarget(m_games[*start].id, ScreenOrigin::Start);
    } else {
        retarget(std::string(), ScreenOrigin::None);
    }
}

bool ScreenPolicy::canPlay(std::string_view id) const {
    const ScreenGame* game = find(id);
    return game != nullptr && isPlayableVideoFile(game->media.video) && !m_failed.contains(id);
}

ScreenPicture ScreenPolicy::picture() const noexcept {
    if (m_targetId.empty()) {
        return ScreenPicture::IdlePattern;
    }
    if (m_videoShowing) {
        return ScreenPicture::Video;
    }
    return m_readyStill.value_or(ScreenPicture::IdlePattern);
}

bool ScreenPolicy::hasVideo() const noexcept {
    return m_settings.clips && !m_targetId.empty() && !m_videoFailed && !m_failed.contains(m_targetId) &&
           isPlayableVideoFile(m_targetMedia.video);
}

std::optional<ScreenPicture> ScreenPolicy::wantedStill() const noexcept {
    if (m_readyStill || m_chainIndex >= m_chain.size()) {
        return std::nullopt;
    }
    // The card stands for a clip that does not play: never drawn over the wait for a clip's first frame (the previous
    // picture stays up meanwhile).
    if (m_chain[m_chainIndex] == ScreenPicture::TitleCard && hasVideo()) {
        return std::nullopt;
    }
    return m_chain[m_chainIndex];
}

void ScreenPolicy::videoShowing() noexcept {
    if (!m_clipShownAt) {
        m_clipShownAt = m_elapsed; // the clip's own clock starts at its first frame
    }
    m_videoShowing = true;
}

void ScreenPolicy::clipDuration(double seconds) noexcept {
    if (seconds > 0.0) {
        m_clipSeconds = seconds;
    }
}

void ScreenPolicy::videoFailed() {
    m_videoFailed = true;
    m_videoShowing = false;
    if (!m_targetId.empty()) {
        m_failed.insert(m_targetId);
        m_playlist.exclude(m_targetId);
    }
}

void ScreenPolicy::stillReady() noexcept {
    if (const std::optional<ScreenPicture> wanted = wantedStill()) {
        m_readyStill = *wanted;
    }
}

void ScreenPolicy::stillFailed() noexcept {
    if (wantedStill()) {
        ++m_chainIndex;
    }
}

void ScreenPolicy::launchStarting(std::optional<double> clipPosition) noexcept {
    if (m_phase == ScreenPhase::Quit) {
        return;
    }
    m_phase = ScreenPhase::Launched;
    m_launchPosition = clipPosition;
    m_resumePosition.reset();
}

void ScreenPolicy::launchEnded() noexcept {
    if (m_phase != ScreenPhase::Launched) {
        return;
    }
    m_phase = ScreenPhase::Running;
    m_resumePosition = std::exchange(m_launchPosition, std::nullopt);
}

std::optional<double> ScreenPolicy::takeResumePosition() noexcept {
    return std::exchange(m_resumePosition, std::nullopt);
}

void ScreenPolicy::retarget(std::string id, ScreenOrigin origin) {
    m_targetId = std::move(id);
    m_origin = m_targetId.empty() ? ScreenOrigin::None : origin;
    const ScreenGame* listed = find(m_targetId);
    m_targetMedia = listed != nullptr ? listed->media : ScreenMedia{};
    m_targetCard = listed != nullptr ? listed->card : ScreenCard{};
    ++m_generation;
    m_chain = stillChain(m_targetMedia);
    if (!m_targetCard.empty()) {
        m_chain.push_back(ScreenPicture::TitleCard); // the chain's end (ADR-0016 §8)
    }
    m_chainIndex = 0;
    m_readyStill.reset();
    m_videoShowing = false;
    m_videoFailed = false;
    m_elapsed = 0.0;
    m_clipShownAt.reset();
    m_clipSeconds.reset();
    m_holdLeft.reset();
    m_launchPosition.reset();
    m_resumePosition.reset();
}

const ScreenGame* ScreenPolicy::find(std::string_view id) const noexcept {
    if (id.empty()) {
        return nullptr;
    }
    const auto found =
        std::find_if(m_games.begin(), m_games.end(), [id](const ScreenGame& game) { return game.id == id; });
    return found != m_games.end() ? &*found : nullptr;
}

bool PictureHold::hold(bool arriving, double nowSeconds, double wallSeconds) noexcept {
    if (!arriving) {
        return false;
    }
    if (!m_since || !m_wallSince) {
        m_since = nowSeconds;
        m_wallSince = wallSeconds;
    }
    // Over only once both clocks ran past the cap.
    return nowSeconds - *m_since < kMaxHeldPictureSeconds || wallSeconds - *m_wallSince < kMaxHeldPictureSeconds;
}

void PictureHold::reset() noexcept {
    m_since.reset();
    m_wallSince.reset();
}

} // namespace rv::app
