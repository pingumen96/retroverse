#include "screen_verdicts.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/utf8.hpp>

#include <spdlog/spdlog.h>

#include <utility>

namespace rv::app {
namespace {

/// How long a write waits for the lock the CLI may hold while it merges its own verdicts.
constexpr std::chrono::seconds kLockWait{2};

} // namespace

ScreenVerdicts::ScreenVerdicts(std::filesystem::path file, bool settle, std::chrono::milliseconds settleBound)
    : m_file(std::move(file)), m_settle(settle), m_settleBound(settleBound) {
    if (m_file.empty()) {
        m_loaded = true;
        return;
    }
    m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

ScreenVerdicts::~ScreenVerdicts() {
    shutdown(std::chrono::seconds(5));
}

void ScreenVerdicts::run(std::stop_token stop) {
    auto read = media::readVideoVerdicts(m_file);
    // Only the failures are kept whole; of the playable clips, their files' facts.
    media::VideoVerdicts failures;
    std::map<std::string, media::VideoFileFacts, std::less<>> playable;
    std::size_t entries = 0;
    if (read) {
        entries = read->size();
        for (const media::VideoVerdict* verdict : read->entries()) {
            if (verdict->playable()) {
                if (std::optional<std::string> key = media::verdictKey(verdict->path)) {
                    playable.emplace(std::move(*key), verdict->file);
                }
            } else {
                static_cast<void>(failures.set(*verdict));
            }
        }
    }
    {
        const std::lock_guard lock(m_mutex);
        m_failures = std::move(failures);
        m_playable = std::move(playable);
        m_record.loaded = read.has_value();
        m_record.entries = entries;
        m_loaded = true;
        m_changed = true;
    }
    m_wake.notify_all();
    if (read) {
        spdlog::info("Video verdicts: {} verdict(s) read from {}", entries, core::pathToUtf8Lossy(m_file));
    } else {
        spdlog::warn("Video verdicts: {} cannot be read ({}); clips are opened whatever their verdicts",
                     core::pathToUtf8Lossy(m_file), read.error().toString());
    }
    for (;;) {
        std::deque<Look> looks;
        std::deque<Pending> batch;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, stop, [this] { return !m_queue.empty() || !m_looks.empty() || m_stopping; });
            if (m_queue.empty() && m_looks.empty()) {
                return; // stopping, everything done
            }
            looks.swap(m_looks);
            batch.swap(m_queue);
            m_busy = true;
        }
        for (const Look& look : looks) {
            lookAt(look);
        }
        write(batch);
        {
            const std::lock_guard lock(m_mutex);
            m_busy = false;
        }
        m_wake.notify_all();
    }
}

void ScreenVerdicts::lookAt(const Look& look) {
    // The file as its folder lists it now: unchanged, the clip is never opened; changed (or gone), it is played again.
    const auto facts = core::listedFileFacts(look.file);
    const bool unchanged = facts && *facts == look.verdict.file;
    {
        const std::lock_guard lock(m_mutex);
        Screening& screening = m_screened[look.key];
        if (screening == Screening::Looking) {
            screening = unchanged ? Screening::KnownBad : Screening::Changed;
            if (unchanged) {
                m_record.knownBad.push_back(VideoVerdictRecord{look.gameId, media::verdictName(look.verdict.failure)});
            } else {
                m_changed = true;
            }
        }
    }
    if (unchanged) {
        spdlog::info("Video: the clip of game {} is known to fail ({}): never opened, its stills shown", look.gameId,
                     media::verdictName(look.verdict.failure));
    } else {
        spdlog::info("Video: the clip of game {} failed before ({}), but its file changed: it is played again",
                     look.gameId, media::verdictName(look.verdict.failure));
    }
}

void ScreenVerdicts::write(std::deque<Pending>& batch) {
    std::vector<media::VideoVerdict> changes;
    std::vector<VideoVerdictRecord> given;
    for (Pending& pending : batch) {
        // The file as its folder lists it now: the verdict's key beside its path.
        auto facts = core::listedFileFacts(pending.file);
        if (!facts) {
            spdlog::info("Video verdicts: a clip's verdict is not kept: its file cannot be looked at ({})",
                         toString(facts.error().code));
            continue;
        }
        pending.verdict.file = *facts;
        if (pending.verdict.playable()) {
            const std::lock_guard lock(m_mutex);
            if (const auto known = m_playable.find(pending.key); known != m_playable.end() && known->second == *facts) {
                continue; // the file says so already, of the file as it is
            }
        }
        given.push_back(VideoVerdictRecord{pending.gameId, media::verdictName(pending.verdict.failure)});
        changes.push_back(std::move(pending.verdict));
    }
    {
        const std::lock_guard lock(m_mutex);
        m_record.given.insert(m_record.given.end(), given.begin(), given.end());
    }
    if (changes.empty()) {
        return;
    }
    const auto merged = media::updateVideoVerdicts(m_file, changes, {}, kLockWait);
    const std::lock_guard lock(m_mutex);
    if (!merged) {
        ++m_record.writeFailures;
        spdlog::warn("Video verdicts: {} verdict(s) not written: {}", changes.size(), merged.error().toString());
        return;
    }
    m_record.written += changes.size();
    for (media::VideoVerdict& change : changes) {
        const std::optional<std::string> key = media::verdictKey(change.path);
        if (!key) {
            continue;
        }
        if (change.playable()) {
            m_playable.insert_or_assign(*key, change.file);
            m_failures.erase(change.path);
        } else {
            m_playable.erase(*key);
            m_screened.insert_or_assign(*key, Screening::KnownBad); // failed in this run: left out from now on
            static_cast<void>(m_failures.set(std::move(change)));
        }
    }
}

bool ScreenVerdicts::idle() const noexcept {
    return m_queue.empty() && m_looks.empty() && !m_busy;
}

void ScreenVerdicts::waitLoaded(std::chrono::milliseconds bound) {
    std::unique_lock lock(m_mutex);
    m_wake.wait_for(lock, bound, [this] { return m_loaded; });
}

bool ScreenVerdicts::takeChanged() {
    const std::lock_guard lock(m_mutex);
    return std::exchange(m_changed, false);
}

std::vector<ScreenGame> ScreenVerdicts::screened(std::vector<ScreenGame> games) {
    std::unique_lock lock(m_mutex);
    if (!m_loaded) {
        return games;
    }
    // This answer holds everything known now: a change seen before it needs no other.
    m_changed = false;
    if (m_failures.empty()) {
        return games;
    }
    // The failure verdicts' clips met for the first time: looked at by the thread, left out meanwhile.
    bool asked = false;
    for (const ScreenGame& game : games) {
        if (game.media.videoKey.empty() || game.media.video.empty()) {
            continue;
        }
        const media::VideoVerdict* failure = m_failures.find(game.media.videoKey);
        const std::optional<std::string> key = media::verdictKey(game.media.videoKey);
        if (failure == nullptr || !key) {
            continue;
        }
        if (m_screened.try_emplace(*key, Screening::Looking).second) {
            m_looks.push_back(Look{*key, game.media.video, game.id, *failure});
            asked = true;
        }
    }
    if (asked) {
        m_wake.notify_all();
        if (m_settle) {
            // A lockstep run gives the same games from its first frame: the looks are done first (a few files' facts).
            m_wake.wait_for(lock, m_settleBound, [this] { return m_looks.empty() && !m_busy; });
            m_changed = false;
        }
    }
    for (ScreenGame& game : games) {
        if (game.media.videoKey.empty() || game.media.video.empty()) {
            continue;
        }
        const std::optional<std::string> key = media::verdictKey(game.media.videoKey);
        if (!key) {
            continue;
        }
        const auto screening = m_screened.find(*key);
        if (screening != m_screened.end() && screening->second != Screening::Changed) {
            game.media.video.clear();
            game.media.videoKey.clear();
        }
    }
    return games;
}

std::optional<std::string> ScreenVerdicts::leftOutFailure(const ScreenMedia& media) const {
    if (media.videoKey.empty() || media.video.empty()) {
        return std::nullopt;
    }
    const std::optional<std::string> key = media::verdictKey(media.videoKey);
    if (!key) {
        return std::nullopt;
    }
    const std::lock_guard lock(m_mutex);
    const auto screening = m_screened.find(*key);
    if (screening == m_screened.end() || screening->second == Screening::Changed) {
        return std::nullopt;
    }
    const media::VideoVerdict* failure = m_failures.find(media.videoKey);
    return failure != nullptr ? std::optional<std::string>(media::verdictName(failure->failure)) : std::nullopt;
}

void ScreenVerdicts::give(const ScreenMedia& media, const std::string& gameId, media::VideoVerdict verdict) {
    if (m_file.empty() || media.videoKey.empty() || media.video.empty()) {
        return;
    }
    std::optional<std::string> key = media::verdictKey(media.videoKey);
    const std::lock_guard lock(m_mutex);
    if (!key || m_stopping || !m_given.insert(*key).second) {
        return; // one verdict per clip and run
    }
    verdict.path = media.videoKey;
    verdict.source = media::VerdictSource::Play;
    m_queue.push_back(Pending{media.video, std::move(*key), gameId, std::move(verdict)});
    m_wake.notify_all();
}

void ScreenVerdicts::clipFailed(const ScreenMedia& media, const std::string& gameId, media::DecodeFailure failure,
                                const std::optional<media::StreamInfo>& info, const media::DecoderStats& stats) {
    // A missing file, or one the system could not read, says nothing of the clip.
    if (failure == media::DecodeFailure::None || failure == media::DecodeFailure::FileNotFound ||
        failure == media::DecodeFailure::ReadError) {
        return;
    }
    media::VideoVerdict verdict;
    verdict.failure = failure;
    if (info) {
        media::describeStream(verdict, *info);
    }
    verdict.decodeErrors = stats.decodeErrors;
    verdict.damagedFrames = stats.damagedFrames;
    give(media, gameId, std::move(verdict));
}

void ScreenVerdicts::clipPlayed(const ScreenMedia& media, const std::string& gameId,
                                const std::optional<media::StreamInfo>& info) {
    // Whether the file holds it already, of the clip's file as it is now, is the thread's to tell (a file looked at).
    media::VideoVerdict verdict;
    if (info) {
        media::describeStream(verdict, *info);
    }
    give(media, gameId, std::move(verdict));
}

void ScreenVerdicts::shutdown(std::chrono::milliseconds bound) {
    if (!m_thread.joinable()) {
        return;
    }
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    {
        std::unique_lock lock(m_mutex);
        if (!m_wake.wait_for(lock, bound, [this] { return m_loaded && idle(); })) {
            spdlog::warn("Video verdicts: not all written within {} ms", bound.count());
        }
    }
    // The thread ends once its write ends (bounded by the lock's wait and the file's write).
    m_thread.request_stop();
    m_thread.join();
}

void ScreenVerdicts::record(VideoVerdictsRecord& record) const {
    const std::lock_guard lock(m_mutex);
    record = m_record;
}

} // namespace rv::app
