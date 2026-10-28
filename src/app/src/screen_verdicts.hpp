#pragma once

// The screens' side of the clips' verdicts (I5 step 7, ADR-0016 §8): <user data>/cache/media/videos.json read on a
// thread of its own when the screens' service starts; a game whose clip has a failure verdict for its file as it is now
// loses its clip before any screen sees it (never opened: its screens show its stills from their first frame); a clip
// that fails on a screen, or plays through once clean, gives a verdict that the same thread merges into the file
// (media::updateVideoVerdicts: the CLI's verdicts kept). Only clips with a key (ScreenMedia::videoKey: under the media
// root) have verdicts. Main thread only, but for its own thread.

#include <rv/app/run_report.hpp>
#include <rv/app/screen_game.hpp>
#include <rv/media/video_decoder.hpp>
#include <rv/media/video_verdicts.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace rv::app {

class ScreenVerdicts {
public:
    /// Starts reading `file` (empty: no verdicts, nothing read or written).
    explicit ScreenVerdicts(std::filesystem::path file);
    ~ScreenVerdicts();
    ScreenVerdicts(const ScreenVerdicts&) = delete;
    ScreenVerdicts& operator=(const ScreenVerdicts&) = delete;

    /// Waits at most `bound` for the file to be read (lockstep runs: the same games from the first frame).
    void waitLoaded(std::chrono::milliseconds bound);
    /// True once, on the first call after the file was read (or found missing or unreadable): the games given before
    /// are screened again.
    [[nodiscard]] bool takeLoaded();

    /// `games` with the clips known to fail left out (their videos and keys cleared), each such game recorded once.
    /// A failure verdict's file is looked at (its size and time) once in the run. Unchanged before the file is read.
    [[nodiscard]] std::vector<ScreenGame> screened(std::vector<ScreenGame> games);

    /// The clip `media` of game `gameId` failed on a screen (a failure of the file: FileNotFound gives none).
    void clipFailed(const ScreenMedia& media, const std::string& gameId, media::DecodeFailure failure,
                    const std::optional<media::StreamInfo>& info, const media::DecoderStats& stats);
    /// The clip played through once from its start without an error or a damaged frame: playable, unless the file
    /// holds that already.
    void clipPlayed(const ScreenMedia& media, const std::string& gameId, const std::optional<media::StreamInfo>& info);

    /// Writes what is queued and ends the thread, waiting at most `bound`. Idempotent.
    void shutdown(std::chrono::milliseconds bound);

    void record(VideoVerdictsRecord& record) const;

private:
    /// A verdict to merge, its file's facts read on the thread.
    struct Pending {
        std::filesystem::path file;
        media::VideoVerdict verdict;
    };

    void run(std::stop_token stop);
    void give(const ScreenMedia& media, const std::string& gameId, media::VideoVerdict verdict);

    std::filesystem::path m_file;
    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    bool m_loaded = false;
    bool m_loadReported = false;
    bool m_stopping = false;
    media::VideoVerdicts m_verdicts;
    std::deque<Pending> m_queue;
    bool m_writing = false;
    VideoVerdictsRecord m_record;
    /// Keys looked at in the run: known to fail (true) or not.
    std::map<std::string, bool, std::less<>> m_screened;
    /// Keys a verdict was given for in the run.
    std::set<std::string, std::less<>> m_given;
    std::jthread m_thread;
};

} // namespace rv::app
