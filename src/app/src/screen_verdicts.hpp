#pragma once

// The screens' side of the clips' verdicts (I5 step 7, ADR-0016 §8): <user data>/cache/media/videos.json read on a
// thread of its own when the screens' service starts; a game whose clip has a failure verdict for its file as it is now
// loses its clip before any screen sees it (never opened: its screens show its stills from their first frame); a clip
// that fails on a screen, or plays through once clean, gives a verdict that the same thread merges into the file
// (media::updateVideoVerdicts: the CLI's verdicts kept). Only clips with a key (ScreenMedia::videoKey: under the media
// root) have verdicts. Main thread only, but for its own thread, which makes every file system call: the file's read
// and writes, and the look at a failure verdict's clip (its size and time, once per clip and run), so that a sleeping
// drive never stalls a frame.
//
// What it keeps of the file: the failure verdicts (a few: 3 of the library's 32,809 clips when ADR-0016 §8 was
// amended), and of the playable ones only their keys and their files' facts (to know that the file says so already).

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
    /// Starts reading `file` (empty: no verdicts, nothing read or written). `settle` (lockstep runs): screened() waits
    /// at most `settleBound` for the looks it asks for, so that a run gives the same games from its first frame.
    ScreenVerdicts(std::filesystem::path file, bool settle, std::chrono::milliseconds settleBound);
    ~ScreenVerdicts();
    ScreenVerdicts(const ScreenVerdicts&) = delete;
    ScreenVerdicts& operator=(const ScreenVerdicts&) = delete;

    /// Waits at most `bound` for the file to be read (lockstep runs: the same games from the first frame).
    void waitLoaded(std::chrono::milliseconds bound);
    /// True once after the file was read (or found missing or unreadable), and once after a clip left out while its
    /// file was looked at turned out to have changed: the games given before are screened again.
    [[nodiscard]] bool takeChanged();

    /// `games` with the clips known to fail left out (their videos and keys cleared). A clip with a failure verdict
    /// whose file was not looked at yet in the run is left out until the thread has looked at it (its size and time):
    /// unchanged, it stays out and its game is recorded once (knownBad); changed, it comes back (takeChanged()). Every
    /// clip is given while the file is not read yet.
    [[nodiscard]] std::vector<ScreenGame> screened(std::vector<ScreenGame> games);

    /// The clip `media` of game `gameId` failed on a screen (a failure of the file: FileNotFound and ReadError give
    /// none).
    void clipFailed(const ScreenMedia& media, const std::string& gameId, media::DecodeFailure failure,
                    const std::optional<media::StreamInfo>& info, const media::DecoderStats& stats);
    /// The clip played through once from its start without an error or a damaged frame: playable, unless the file
    /// holds that already for the clip's file as it is now (media::VideoVerdicts::findCurrent()'s rule).
    void clipPlayed(const ScreenMedia& media, const std::string& gameId, const std::optional<media::StreamInfo>& info);

    /// Writes what is queued and ends the thread, waiting at most `bound`. Idempotent.
    void shutdown(std::chrono::milliseconds bound);

    void record(VideoVerdictsRecord& record) const;

private:
    /// A verdict to merge, its file's facts read on the thread.
    struct Pending {
        std::filesystem::path file;
        std::string key;
        std::string gameId;
        media::VideoVerdict verdict;
    };
    /// A failure verdict's clip to look at: is its file still the one judged?
    struct Look {
        std::string key;
        std::filesystem::path file;
        std::string gameId;
        media::VideoVerdict verdict;
    };
    /// A failure verdict's clip in the run.
    enum class Screening : std::uint8_t {
        Looking,  ///< its file is being looked at: left out meanwhile
        KnownBad, ///< unchanged (or it failed in this run): left out
        Changed,  ///< its file changed: given to the screens, played and judged again
    };

    void run(std::stop_token stop);
    void lookAt(const Look& look);
    void write(std::deque<Pending>& batch);
    void give(const ScreenMedia& media, const std::string& gameId, media::VideoVerdict verdict);
    /// Nothing queued, nothing being done (the lock held).
    [[nodiscard]] bool idle() const noexcept;

    std::filesystem::path m_file;
    const bool m_settle;
    const std::chrono::milliseconds m_settleBound;
    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    bool m_loaded = false;
    bool m_changed = false;
    bool m_stopping = false;
    /// The failure verdicts, and the playable clips' files (folded key to facts).
    media::VideoVerdicts m_failures;
    std::map<std::string, media::VideoFileFacts, std::less<>> m_playable;
    std::deque<Pending> m_queue;
    std::deque<Look> m_looks;
    bool m_busy = false;
    VideoVerdictsRecord m_record;
    /// The failure verdicts' clips met in the run (folded keys).
    std::map<std::string, Screening, std::less<>> m_screened;
    /// Keys a verdict was given for in the run.
    std::set<std::string, std::less<>> m_given;
    std::jthread m_thread;
};

} // namespace rv::app
