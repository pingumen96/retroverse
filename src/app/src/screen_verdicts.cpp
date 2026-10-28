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

ScreenVerdicts::ScreenVerdicts(std::filesystem::path file) : m_file(std::move(file)) {
    if (m_file.empty()) {
        m_loaded = true;
        m_loadReported = true;
        return;
    }
    m_thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

ScreenVerdicts::~ScreenVerdicts() {
    shutdown(std::chrono::seconds(5));
}

void ScreenVerdicts::run(std::stop_token stop) {
    auto read = media::readVideoVerdicts(m_file);
    std::size_t entries = 0;
    {
        const std::lock_guard lock(m_mutex);
        if (read) {
            m_verdicts = std::move(*read);
            m_record.loaded = true;
            m_record.entries = m_verdicts.size();
            entries = m_verdicts.size();
        }
        m_loaded = true;
    }
    m_wake.notify_all();
    if (read) {
        spdlog::info("Video verdicts: {} verdict(s) read from {}", entries, core::pathToUtf8Lossy(m_file));
    } else {
        spdlog::warn("Video verdicts: {} cannot be read ({}); clips are opened whatever their verdicts",
                     core::pathToUtf8Lossy(m_file), read.error().toString());
    }
    for (;;) {
        std::deque<Pending> batch;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, stop, [this] { return !m_queue.empty() || m_stopping; });
            if (m_queue.empty()) {
                return; // stopping, everything written
            }
            batch.swap(m_queue);
            m_writing = true;
        }
        std::vector<media::VideoVerdict> changes;
        for (Pending& pending : batch) {
            // The file as its folder lists it now: the verdict's key beside its path.
            auto facts = core::listedFileFacts(pending.file);
            if (!facts) {
                spdlog::info("Video verdicts: a clip's verdict is not kept: its file cannot be looked at ({})",
                             toString(facts.error().code));
                continue;
            }
            pending.verdict.file = *facts;
            changes.push_back(std::move(pending.verdict));
        }
        auto merged = changes.empty() ? Expected<media::VideoVerdicts>(media::VideoVerdicts{})
                                      : media::updateVideoVerdicts(m_file, changes, {}, kLockWait);
        {
            const std::lock_guard lock(m_mutex);
            m_writing = false;
            if (!merged) {
                ++m_record.writeFailures;
                spdlog::warn("Video verdicts: {} verdict(s) not written: {}", changes.size(),
                             merged.error().toString());
            } else {
                m_record.written += changes.size();
                for (media::VideoVerdict& change : changes) {
                    static_cast<void>(m_verdicts.set(std::move(change)));
                }
            }
        }
        m_wake.notify_all();
    }
}

void ScreenVerdicts::waitLoaded(std::chrono::milliseconds bound) {
    std::unique_lock lock(m_mutex);
    m_wake.wait_for(lock, bound, [this] { return m_loaded; });
}

bool ScreenVerdicts::takeLoaded() {
    const std::lock_guard lock(m_mutex);
    if (!m_loaded || m_loadReported) {
        return false;
    }
    m_loadReported = true;
    return true;
}

std::vector<ScreenGame> ScreenVerdicts::screened(std::vector<ScreenGame> games) {
    const std::lock_guard lock(m_mutex);
    if (!m_loaded) {
        return games;
    }
    // A screen asking later gets the same answer: the report's knownBad comes in once.
    m_loadReported = true;
    if (m_verdicts.empty()) {
        return games;
    }
    for (ScreenGame& game : games) {
        if (game.media.videoKey.empty() || game.media.video.empty()) {
            continue;
        }
        const std::optional<std::string> key = media::verdictKey(game.media.videoKey);
        if (!key) {
            continue;
        }
        bool knownBad = false;
        if (const auto seen = m_screened.find(*key); seen != m_screened.end()) {
            knownBad = seen->second;
        } else {
            const media::VideoVerdict* verdict = m_verdicts.find(game.media.videoKey);
            if (verdict != nullptr && !verdict->playable()) {
                // Only a failure's file is looked at: unchanged, the clip is never opened; changed, it is checked again
                // by playing it.
                const auto facts = core::listedFileFacts(game.media.video);
                knownBad = facts && *facts == verdict->file;
                if (knownBad) {
                    m_record.knownBad.push_back(VideoVerdictRecord{game.id, media::verdictName(verdict->failure)});
                    spdlog::info("Video: the clip of game {} is known to fail ({}): never opened, its stills shown",
                                 game.id, media::verdictName(verdict->failure));
                }
            }
            m_screened.emplace(*key, knownBad);
        }
        if (knownBad) {
            game.media.video.clear();
            game.media.videoKey.clear();
        }
    }
    return games;
}

void ScreenVerdicts::give(const ScreenMedia& media, const std::string& gameId, media::VideoVerdict verdict) {
    if (m_file.empty() || media.videoKey.empty() || media.video.empty()) {
        return;
    }
    const std::optional<std::string> key = media::verdictKey(media.videoKey);
    const std::lock_guard lock(m_mutex);
    if (!key || m_stopping || !m_given.insert(*key).second) {
        return; // one verdict per clip and run
    }
    m_record.given.push_back(VideoVerdictRecord{gameId, media::verdictName(verdict.failure)});
    verdict.path = media.videoKey;
    verdict.source = media::VerdictSource::Play;
    m_queue.push_back(Pending{media.video, std::move(verdict)});
    m_wake.notify_all();
}

void ScreenVerdicts::clipFailed(const ScreenMedia& media, const std::string& gameId, media::DecodeFailure failure,
                                const std::optional<media::StreamInfo>& info, const media::DecoderStats& stats) {
    if (failure == media::DecodeFailure::None || failure == media::DecodeFailure::FileNotFound) {
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
    {
        const std::lock_guard lock(m_mutex);
        const media::VideoVerdict* known = m_verdicts.find(media.videoKey);
        if (known != nullptr && known->playable()) {
            return; // the file says so already
        }
    }
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
        if (!m_wake.wait_for(lock, bound, [this] { return m_loaded && m_queue.empty() && !m_writing; })) {
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
