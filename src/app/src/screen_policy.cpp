#include <rv/app/screen_policy.hpp>
#include <rv/core/utf8.hpp>

#include <algorithm>
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
        break;
    }
    return nullptr;
}

std::optional<std::size_t> chooseStartGame(std::span<const ScreenGame> games) {
    std::optional<std::size_t> played;
    std::optional<std::size_t> first;
    for (std::size_t index = 0; index < games.size(); ++index) {
        const ScreenGame& game = games[index];
        if (!isPlayableVideoFile(game.media.video)) {
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

ScreenPolicy::ScreenPolicy(std::vector<ScreenGame> games) : m_games(std::move(games)) {
    if (const std::optional<std::size_t> start = chooseStartGame(m_games)) {
        retarget(m_games[*start].id);
    }
    m_generation = 0; // the start choice is the first target
}

void ScreenPolicy::setGames(std::vector<ScreenGame> games) {
    m_games = std::move(games);
    if (!m_selected) {
        const std::optional<std::size_t> start = chooseStartGame(m_games);
        std::string id = start ? m_games[*start].id : std::string();
        if (id != m_targetId) {
            retarget(std::move(id));
            return;
        }
    }
    const ScreenGame* listed = find(m_targetId);
    const ScreenMedia media = listed != nullptr ? listed->media : ScreenMedia{};
    if (!m_targetId.empty() && media != m_targetMedia) {
        retarget(m_targetId); // its media changed: shown afresh
    }
}

void ScreenPolicy::hover(std::optional<std::string_view> id, double nowSeconds) {
    if (!id || id->empty()) {
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
    m_selected = true;
    if (m_hoverId != m_targetId) {
        retarget(m_hoverId);
    }
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
    return !m_targetId.empty() && !m_videoFailed && isPlayableVideoFile(m_targetMedia.video);
}

std::optional<ScreenPicture> ScreenPolicy::wantedStill() const noexcept {
    if (m_readyStill || m_chainIndex >= m_chain.size()) {
        return std::nullopt;
    }
    return m_chain[m_chainIndex];
}

void ScreenPolicy::videoFailed() noexcept {
    m_videoFailed = true;
    m_videoShowing = false;
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

void ScreenPolicy::retarget(std::string id) {
    m_targetId = std::move(id);
    const ScreenGame* listed = find(m_targetId);
    m_targetMedia = listed != nullptr ? listed->media : ScreenMedia{};
    ++m_generation;
    m_chain = stillChain(m_targetMedia);
    m_chainIndex = 0;
    m_readyStill.reset();
    m_videoShowing = false;
    m_videoFailed = false;
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

} // namespace rv::app
