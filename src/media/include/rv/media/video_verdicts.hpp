#pragma once

// The clips' verdicts (I5 step 7, ADR-0016 §8): <user data>/cache/media/videos.json, Retroverse's own memory of which
// clips of the media root play and which fail, so that a clip known to fail is never opened again while its file is
// unchanged. One verdict per clip, keyed by its path under the media root (compared as the file system compares names:
// core::fileSystemUpperCase) and the file's size and last write time as its folder lists them
// (core::listedFileFacts, core::walkTree): a file of another size or time has no verdict and is checked again.
// Written by the app when a clip fails or plays through once clean, and by `retroverse-cli media --check-videos`;
// both merge their verdicts into the file as it is when they write (updateVideoVerdicts(), under a lock shared by the
// processes), so that neither drops the other's. Never written under the media root: it lives in Retroverse's cache.
//
// The file is canonical JSON (core::writeCanonicalJson: integers only, members sorted, one verdict per line):
//   {"format": "retroverse-video-verdicts", "version": 1, "videos": [
//    {"audioSampleRate": 96000, "bytes": 1234567, "codec": "h264", "damagedFrames": 0, "decodeErrors": 0,
//     "durationMs": 38000, "height": 480, "modified": 133700000000000000, "path": "snes/videos/Game (USA).mp4",
//     "source": "check", "verdict": "playable", "width": 640}]}
// "verdict" is "playable" or the decoder's failure ("invalid-file", "no-video-stream", "unsupported-codec",
// "corrupt-data", "truncated", "decoder-error"); "modified" the last write time in FILETIME ticks; "durationMs",
// "width", "height", "audioSampleRate" and "codec" what the decoder found (0 and "" when it found nothing); "source"
// "check" (the CLI's sample) or "play" (the app). The verdicts are sorted by their folded paths.
//
// A verdict lasts until its file changes (another size or last write time). Deleting the file forgets every verdict
// (each clip is checked or played again); `retroverse-cli media --check-videos --recheck-failures` checks the clips
// with a failure verdict again. The version names the verdicts' meaning as well as their form: a change of the decoder
// that may judge a clip otherwise (an FFmpeg upgrade, a threshold of ADR-0004) raises kVideoVerdictsVersion, and a file
// of another version is replaced by the next write (every clip checked again).

#include <rv/core/expected.hpp>
#include <rv/core/file_io.hpp>
#include <rv/media/video_decoder.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rv::media {

/// The file's "format" and "version".
inline constexpr std::string_view kVideoVerdictsFormat = "retroverse-video-verdicts";
inline constexpr std::int64_t kVideoVerdictsVersion = 1;

/// A clip's size and last write time (core::listedFileFacts()): what keys its verdict beside its path.
using VideoFileFacts = core::SharedFileReader::Facts;

/// Who gave a verdict.
enum class VerdictSource : std::uint8_t {
    Check, ///< retroverse-cli media --check-videos: a sample of the clip decoded (its head, middle and end)
    Play,  ///< the app: the clip failed on a screen, or played through once clean
};

/// "check", "play".
[[nodiscard]] std::string_view toString(VerdictSource source) noexcept;

/// "playable" for DecodeFailure::None, else the failure's name with dashes ("corrupt-data"), as the run report names
/// the decoders' failures.
[[nodiscard]] std::string verdictName(DecodeFailure failure);
/// The failure a verdict names (None for "playable"); nothing for another text, "file-not-found" and "read-error"
/// included (a missing file, or one the system could not read, has no verdict).
[[nodiscard]] std::optional<DecodeFailure> parseVerdictName(std::string_view name);

/// One clip's verdict.
struct VideoVerdict {
    /// Below the media root, '/' between the elements, as its folder spells it (or as the catalog derives it).
    std::string path;
    VideoFileFacts file;
    /// None: playable.
    DecodeFailure failure = DecodeFailure::None;
    /// What the decoder found: the video stream's duration (milliseconds), its coded size, the first audio stream's
    /// rate, the video codec; 0 and "" for what it did not find (a file that does not open).
    std::int64_t durationMs = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
    std::int64_t audioSampleRate = 0;
    std::string codec;
    /// Errors and damaged frames the decoder counted in what it read (ADR-0004's thresholds: a playable clip may have a
    /// few).
    std::uint64_t decodeErrors = 0;
    std::uint64_t damagedFrames = 0;
    VerdictSource source = VerdictSource::Check;

    [[nodiscard]] bool playable() const noexcept { return failure == DecodeFailure::None; }

    bool operator==(const VideoVerdict&) const = default;
};

/// What the decoder found, for a verdict: the stream's duration, size, audio rate and codec.
void describeStream(VideoVerdict& verdict, const StreamInfo& info);

/// The verdicts of one media root, by path (folded: "SNES/Videos/A.MP4" is "snes/videos/a.mp4").
class VideoVerdicts {
public:
    /// Reads a file's text: canonical JSON of the form above (core::parseCanonicalJson: a file edited by hand is
    /// refused). Errors: ParseError, ValidationFailed (another format or version, a field missing or out of range, an
    /// unknown verdict or source, two verdicts of one path), `context` the file.
    [[nodiscard]] static Expected<VideoVerdicts> parse(std::string_view text, std::string_view context);
    /// The canonical text (byte-identical to what parse() read when nothing changed). Errors: InvalidArgument for a
    /// path that is no UTF-8 (never, for verdicts set()).
    [[nodiscard]] Expected<std::string> format() const;

    /// The verdict of `path` whatever its file's facts; null when none.
    [[nodiscard]] const VideoVerdict* find(std::string_view path) const;
    /// The verdict of `path` given while its file had `facts`; null when none, or when the file changed since (it is
    /// checked again).
    [[nodiscard]] const VideoVerdict* findCurrent(std::string_view path, const VideoFileFacts& facts) const;
    /// Sets the verdict of its path, replacing the one it had. Errors: InvalidArgument for an empty or malformed path
    /// (not UTF-8, "\" in it, an empty, "." or ".." element) or a FileNotFound or ReadError failure (a missing file,
    /// or one the system could not read, has none: the cause is not the clip's).
    Status set(VideoVerdict verdict);
    /// Removes the verdict of `path`; false when it had none.
    bool erase(std::string_view path);

    /// Every verdict, by folded path.
    [[nodiscard]] std::vector<const VideoVerdict*> entries() const;
    [[nodiscard]] std::size_t size() const noexcept { return m_byKey.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_byKey.empty(); }

    bool operator==(const VideoVerdicts&) const = default;

private:
    /// Folded path to its verdict.
    std::map<std::string, VideoVerdict, std::less<>> m_byKey;
};

/// The folded form of a verdict's path (its key); nothing for a malformed one.
[[nodiscard]] std::optional<std::string> verdictKey(std::string_view path);

/// <cache dir>/media/videos.json.
[[nodiscard]] std::filesystem::path videoVerdictsFile(const std::filesystem::path& cacheDir);

/// Reads the verdicts file under the lock its writers share (updateVideoVerdicts()), waiting at most `lockWait` for
/// it: a writer's atomic replace (a rename over the file) fails while another program holds the file open, whatever
/// that program shares. A file that does not exist gives none. Errors: Conflict when the lock is not had in time, those
/// of reading the file (core::readFileShared's), and of parse() (ParseError, ValidationFailed: isUnreadableText()).
[[nodiscard]] Expected<VideoVerdicts> readVideoVerdicts(const std::filesystem::path& file,
                                                        std::chrono::milliseconds lockWait = std::chrono::seconds(10));

/// True for the errors of a verdicts file whose text is not the verdicts' (ParseError, ValidationFailed): the file was
/// read, and the next write replaces it. Any other error of reading it leaves it as it is.
[[nodiscard]] bool isUnreadableText(ErrorCode code) noexcept;

/// Merges `changes` (each replacing the verdict of its path) and `removals` (paths whose verdicts go) into the file as
/// it is now, and writes it atomically (core::writeTextFileAtomic), under a lock the processes share (a named mutex of
/// the file's path) so that the app and the CLI never drop each other's verdicts. A file whose text is not the
/// verdicts' (isUnreadableText()) is replaced (logged); a file that cannot be read is left as it is and nothing is
/// written. Returns what was written. Errors: Conflict when the lock is not had within `lockWait`, those of reading the
/// file (not its parse), those of set() for a malformed change, those of writing the file.
[[nodiscard]] Expected<VideoVerdicts>
updateVideoVerdicts(const std::filesystem::path& file, std::span<const VideoVerdict> changes,
                    std::span<const std::string> removals = {},
                    std::chrono::milliseconds lockWait = std::chrono::seconds(10));

} // namespace rv::media
