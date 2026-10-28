#include <rv/media/video_decoder.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <windows.h>

#include <rv/core/text.hpp>
#include <rv/media/output_size.hpp>

#include "ffmpeg_support.hpp"

extern "C" {
#include <libavutil/parseutils.h>
}

namespace rv::media {

std::string_view toString(DecoderState state) noexcept {
    switch (state) {
    case DecoderState::Opening:
        return "opening";
    case DecoderState::Playing:
        return "playing";
    case DecoderState::Paused:
        return "paused";
    case DecoderState::Ended:
        return "ended";
    case DecoderState::Failed:
        return "failed";
    case DecoderState::Stopped:
        return "stopped";
    }
    return "unknown";
}

std::string_view toString(DecodeFailure failure) noexcept {
    switch (failure) {
    case DecodeFailure::None:
        return "none";
    case DecodeFailure::FileNotFound:
        return "file not found";
    case DecodeFailure::InvalidFile:
        return "invalid file";
    case DecodeFailure::NoVideoStream:
        return "no video stream";
    case DecodeFailure::UnsupportedCodec:
        return "unsupported codec";
    case DecodeFailure::CorruptData:
        return "corrupt data";
    case DecodeFailure::Truncated:
        return "truncated";
    case DecodeFailure::DecoderError:
        return "decoder error";
    case DecodeFailure::ReadError:
        return "read error";
    }
    return "unknown";
}

ErrorCode errorCodeFor(DecodeFailure failure) noexcept {
    switch (failure) {
    case DecodeFailure::None:
        return ErrorCode::Unknown;
    case DecodeFailure::FileNotFound:
    case DecodeFailure::NoVideoStream:
        return ErrorCode::NotFound;
    case DecodeFailure::InvalidFile:
    case DecodeFailure::CorruptData:
    case DecodeFailure::Truncated:
        return ErrorCode::ParseError;
    case DecodeFailure::UnsupportedCodec:
        return ErrorCode::Unsupported;
    case DecodeFailure::DecoderError:
    case DecodeFailure::ReadError:
        return ErrorCode::IoError;
    }
    return ErrorCode::Unknown;
}

namespace {

using Clock = std::chrono::steady_clock;

/// A pass shorter than this share of the declared duration, less kTruncationSlackSeconds, means the file was cut short.
constexpr double kTruncationRatio = kTruncatedPassRatio;
constexpr double kTruncationSlackSeconds = kTruncatedPassSlackSeconds;
/// Frame duration when neither the frame nor the stream tells one.
constexpr double kFallbackFrameSeconds = 1.0 / 30.0;
/// Smallest step between two frames whose timestamps do not increase (broken files).
constexpr double kTimestampNudgeSeconds = 0.001;
/// A seek positions the demuxer this much before its target. MP4 indexes key frames by decoding time, which runs
/// ahead of the presentation time by the reordering delay of B-frames (a few frames, under a second): positioned on
/// the key frame at or before the target itself, the frames between the target and that key frame's presentation time
/// would be lost (B-frames that need the previous key frame). The frames decoded before the target are dropped.
constexpr double kSeekPrerollSeconds = 1.0;

double milliseconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>(duration).count();
}

std::int64_t nanosecondsSince(Clock::time_point start) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

double fileTimeSeconds(const FILETIME& time) noexcept {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<double>(value.QuadPart) * 1e-7; // 100 ns units
}

/// User + kernel CPU time of a thread, seconds; 0 when it cannot be read.
double threadCpuSecondsOf(HANDLE thread) noexcept {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetThreadTimes(thread, &creation, &exit, &kernel, &user) == 0) {
        return 0.0;
    }
    return fileTimeSeconds(kernel) + fileTimeSeconds(user);
}

/// swscale's coefficient table for a stream's matrix; unspecified matrices follow the usual convention (BT.709 from
/// 720 rows up, BT.601 below).
int swsColorspace(AVColorSpace colorspace, int height) noexcept {
    switch (colorspace) {
    case AVCOL_SPC_BT709:
        return SWS_CS_ITU709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M:
        return SWS_CS_SMPTE240M;
    case AVCOL_SPC_FCC:
        return SWS_CS_FCC;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return SWS_CS_BT2020;
    default:
        return height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

/// True for the deprecated full-range "J" formats, whose range the frame may not state.
bool isFullRangeFormat(int format) noexcept {
    switch (format) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
    case AV_PIX_FMT_YUVJ411P:
        return true;
    default:
        return false;
    }
}

std::string pixelFormatName(int format) {
    const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(format));
    return name != nullptr ? std::string(name) : std::string("unknown");
}

double rationalOrZero(AVRational value) noexcept {
    return value.num > 0 && value.den > 0 ? av_q2d(value) : 0.0;
}

/// Duration of `stream` from the per-track "DURATION" tag that Matroska and WebM muxers write (those containers give no
/// stream duration of their own), seconds; 0 when the tag is absent or unreadable.
double taggedDurationSeconds(const AVStream& stream) noexcept {
    const AVDictionaryEntry* tag = av_dict_get(stream.metadata, "DURATION", nullptr, 0);
    if (tag == nullptr || tag->value == nullptr) {
        return 0.0;
    }
    std::int64_t microseconds = 0;
    if (av_parse_time(&microseconds, tag->value, 1) < 0 || microseconds <= 0) {
        return 0.0;
    }
    return static_cast<double>(microseconds) * 1e-6;
}

/// True for the errors the system gives when it cannot open or read a file (FFmpeg's file protocol passes errno on):
/// access denied or a sharing violation, a device error, a busy or missing device, too many open files, a timeout.
/// Never a fault of the file's content, which FFmpeg reports with codes of its own (AVERROR_INVALIDDATA, ...).
bool isSystemReadError(int code) noexcept {
    return code == AVERROR(EACCES) || code == AVERROR(EPERM) || code == AVERROR(EIO) || code == AVERROR(EBUSY) ||
           code == AVERROR(ENXIO) || code == AVERROR(ENODEV) || code == AVERROR(EMFILE) || code == AVERROR(ENFILE) ||
           code == AVERROR(ETIMEDOUT);
}

std::wstring threadDescription(const std::string& name) {
    return core::pathFromUtf8("rv video: " + name).wstring();
}

/// The threads of stopped decoders: VideoDecoder::stop() hands its thread over instead of waiting for it. A thread is
/// joined once it has ended (which then takes no time), at the next hand-over, or by joinAll().
class StoppedThreads {
public:
    StoppedThreads() {
        // The default logger is created first and so destroyed after this object: threads still ending at process
        // exit can log.
        static_cast<void>(spdlog::default_logger_raw());
    }
    StoppedThreads(const StoppedThreads&) = delete;
    StoppedThreads& operator=(const StoppedThreads&) = delete;
    ~StoppedThreads() { joinAll(); }

    /// Keeps `thread` (asked to stop already) until it ends; joins the threads that have ended meanwhile.
    void adopt(std::jthread thread) {
        std::vector<std::jthread> ended;
        {
            const std::lock_guard lock(m_mutex);
            const auto firstEnded = std::partition(m_threads.begin(), m_threads.end(), [](std::jthread& candidate) {
                return WaitForSingleObject(static_cast<HANDLE>(candidate.native_handle()), 0) != WAIT_OBJECT_0;
            });
            ended.assign(std::make_move_iterator(firstEnded), std::make_move_iterator(m_threads.end()));
            m_threads.erase(firstEnded, m_threads.end());
            m_threads.push_back(std::move(thread));
        }
        for (std::jthread& done : ended) {
            done.join(); // ended: returns at once
        }
    }

    /// Waits for every thread kept.
    void joinAll() {
        std::vector<std::jthread> threads;
        {
            const std::lock_guard lock(m_mutex);
            threads.swap(m_threads);
        }
        for (std::jthread& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    std::mutex m_mutex;
    std::vector<std::jthread> m_threads;
};

StoppedThreads& stoppedThreads() {
    static StoppedThreads threads;
    return threads;
}

} // namespace

// --- Shared state
// ------------------------------------------------------------------------------------------------------

struct VideoDecoder::Shared {
    Shared(std::filesystem::path filePath, DecoderSettings decoderSettings)
        : path(std::move(filePath)), settings(std::move(decoderSettings)),
          pool(FramePool::create(settings.queueCapacity + 2)), queue(settings.queueCapacity),
          decodeTimes(settings.timingCapacity), convertTimes(settings.timingCapacity) {}

    const std::filesystem::path path;
    const DecoderSettings settings;
    std::shared_ptr<FramePool> pool;
    FrameQueue queue;

    /// Opening, Playing, Ended, Failed or Stopped; VideoDecoder::state() reports Paused from `paused`.
    std::atomic<DecoderState> state{DecoderState::Opening};
    std::atomic<DecodeFailure> failure{DecodeFailure::None};

    mutable std::mutex infoMutex;
    std::optional<StreamInfo> info;
    std::optional<Error> error;

    /// pause(), resume() and seek() change what follows under controlMutex and notify `control`, which the thread
    /// waits on; `paused` and `generation` are also read without the lock.
    std::mutex controlMutex;
    std::condition_variable_any control;
    std::atomic<bool> paused{false};
    std::optional<double> seekRequest;
    /// Generation of the latest seek(): the thread drops the frames of older ones.
    std::atomic<std::uint32_t> generation{0};

    std::atomic<std::uint64_t> packetsRead{0};
    std::atomic<std::uint64_t> framesDecoded{0};
    std::atomic<std::uint64_t> framesQueued{0};
    std::atomic<std::uint64_t> framesSkipped{0};
    std::atomic<std::uint64_t> decodeErrors{0};
    std::atomic<std::uint64_t> damagedFrames{0};
    std::atomic<std::uint64_t> corruptPackets{0};
    std::atomic<std::uint64_t> loops{0};
    std::atomic<std::uint64_t> seeks{0};
    std::atomic<std::uint64_t> frameAllocations{0};
    std::atomic<std::int64_t> queueWaitNanoseconds{0};
    /// The sound (DecoderSettings::audioEnabled, setAudioEnabled()) and its counters.
    std::atomic<bool> audioEnabled{true};
    std::atomic<std::uint64_t> audioFramesWritten{0};
    std::atomic<std::uint64_t> audioPacketsDiscarded{0};
    std::atomic<std::uint64_t> audioDecodeErrors{0};
    /// start() time, and the delays from it to the stream being open and to the first frame queued (-1: not yet).
    const Clock::time_point started = Clock::now();
    std::atomic<std::int64_t> openNanoseconds{-1};
    std::atomic<std::int64_t> firstFrameNanoseconds{-1};

    mutable std::mutex timingMutex;
    SampleRecorder decodeTimes;
    SampleRecorder convertTimes;

    /// Moves to `next` unless the decoder already failed or stopped (those states are final).
    void setState(DecoderState next) noexcept {
        DecoderState current = state.load();
        while (current != DecoderState::Failed && current != DecoderState::Stopped &&
               !state.compare_exchange_weak(current, next)) {
        }
    }
};

// --- Worker: everything that runs on the decoder thread
// ----------------------------------------------------------------

class VideoDecoder::Worker {
public:
    Worker(Shared& shared, std::stop_token stop) : m_shared(shared), m_stop(std::move(stop)) {}

    /// The thread's body: open, decode until stopped or failed. Never throws.
    void run() noexcept;
    /// Opens the file and the decoder and publishes the StreamInfo. False on failure (recorded) or stop.
    [[nodiscard]] bool open();

private:
    struct SeekRequest {
        double seconds = 0.0;
        std::uint32_t generation = 0;
    };

    static int interruptCallback(void* opaque) noexcept {
        return static_cast<const Worker*>(opaque)->m_stop.stop_requested() ? 1 : 0;
    }

    void decodeLoop();
    /// Reads one packet and decodes it. False ends the loop (stop or failure).
    [[nodiscard]] bool step();
    [[nodiscard]] bool sendPacket(const AVPacket* packet);
    [[nodiscard]] bool receiveFrames();
    /// Converts the decoded frame and queues it. False on stop or failure.
    [[nodiscard]] bool emitFrame();
    /// Drains the decoder at the end of the file, then loops, ends or fails.
    [[nodiscard]] bool endOfStream();
    [[nodiscard]] bool endOfPass();
    /// Seeks to `seconds` into the stream (from its start time) and flushes the decoder. False when seeking fails.
    [[nodiscard]] bool seekTo(double seconds);
    [[nodiscard]] bool applySeek(const SeekRequest& request);
    /// Counts an FFmpeg error (decodeErrors) and notes it (noteError).
    [[nodiscard]] bool decodeError(int code, std::string_view what);
    /// Notes an error or a damaged frame; false (and the decoder fails as CorruptData) when too many came in a row or
    /// within the corruption window.
    [[nodiscard]] bool noteError(std::string_view what);
    /// Waits while paused, unless the first frame after a seek is still to come; processes nothing else. False on
    /// stop.
    [[nodiscard]] bool waitWhilePaused();
    /// Waits at the end of a stream that does not loop, until a seek or the stop. False on stop.
    [[nodiscard]] bool waitAfterEnd();
    [[nodiscard]] std::optional<SeekRequest> takeSeekRequest();
    /// A seek newer than the frames being decoded waits to be carried out: those frames are dropped.
    [[nodiscard]] bool seekPending() const noexcept {
        return m_shared.generation.load(std::memory_order_acquire) != m_generation;
    }
    [[nodiscard]] bool ensureConverter(const AVFrame& frame);
    void resetPass(bool fromStart) noexcept;
    /// The system's error behind the latest open, read or seek: `result` itself when it is one (isSystemReadError),
    /// else the error the file's I/O context recorded (a read the system failed, whatever the demuxer made of it); 0
    /// when neither is.
    [[nodiscard]] int systemReadError(int result) const noexcept;
    /// Finds the first audio stream and, with a sink, opens its decoder (a failure leaves the clip silent).
    void openAudio(StreamInfo& info);
    /// An audio packet: decoded into the sink while the sound is enabled, discarded otherwise.
    void audioPacket(const AVPacket& packet);
    /// Converts a decoded audio frame to the sink's rate, stereo float, and writes it.
    void emitAudio(const AVFrame& frame);
    /// The end of a pass: the frames the audio decoder still holds, then the resampler's tail, written.
    void drainAudio();
    /// Writes `frames` converted frames (m_audioOutChannels each) whose first shows at `ptsSeconds`, up-mixed to stereo
    /// and trimmed at the picture's end (audioEndSeconds), so that every loop starts in sync.
    void writeAudio(const float* converted, std::size_t frames, double ptsSeconds);
    /// The end of the picture on the pass's timeline (relative to the video stream's start): the last pass's end once
    /// one ended, else the video stream's declared duration (or the frames shown so far when longer); none when unknown.
    [[nodiscard]] std::optional<double> audioEndSeconds() const noexcept;
    /// A discontinuity of the sound (a seek, a loop, the sound enabled again): the audio decoder flushed, the
    /// resampler's buffered samples dropped.
    void resetAudio() noexcept;
    void fail(DecodeFailure failure, std::string message);

    Shared& m_shared;
    std::stop_token m_stop;

    detail::InputFormatPtr m_format;
    detail::CodecContextPtr m_codec;
    detail::FramePtr m_frame;
    detail::PacketPtr m_packet;
    detail::SwsPtr m_sws;

    int m_streamIndex = -1;
    AVRational m_timeBase{0, 1};
    bool m_timestampsUsable = false;
    std::int64_t m_startTimestamp = 0;
    double m_nominalDuration = kFallbackFrameSeconds;
    /// The video stream's own duration (stream duration, or the Matroska track's DURATION tag), 0 when unknown: what
    /// a first pass is checked against for a cut (never the container's, which covers a longer audio stream too).
    double m_streamDuration = 0.0;
    /// m_streamDuration, or the container's duration when the stream gives none (StreamInfo, messages).
    double m_declaredDuration = 0.0;
    double m_streamSampleAspect = 1.0;

    // Conversion.
    FrameSize m_sourceSize;
    int m_sourceFormat = -1;
    int m_sourceColorspace = -1;
    int m_sourceRange = -1;
    double m_sourceSampleAspect = 0.0;
    FrameSize m_outputSize;

    // Timestamps.
    double m_loopOffset = 0.0;
    std::optional<double> m_passStart;
    double m_passEnd = 0.0;
    std::uint64_t m_passFrames = 0;
    bool m_passFromStart = true;
    bool m_firstPass = true;
    std::optional<double> m_previousRelative;
    double m_previousDuration = kFallbackFrameSeconds;
    double m_lastPts = -std::numeric_limits<double>::infinity();
    std::optional<double> m_seekTarget;
    std::uint64_t m_serial = 0;
    std::uint32_t m_loop = 0;
    std::uint32_t m_generation = 0;
    /// A seek was carried out and its first frame has not reached the queue yet: it is decoded even while paused.
    bool m_seekFramePending = false;

    // Errors and end of stream.
    int m_consecutiveErrors = 0;
    /// Outcome of the last 64 decoding events (a frame decoded, a packet or frame that failed), newest in bit 0: 1 for
    /// an error or a damaged frame.
    std::uint64_t m_errorHistory = 0;
    /// A clean frame came out of the decoder in this pass (errors then count towards the corruption window).
    bool m_decodedClean = false;
    bool m_truncatedRead = false;
    bool m_ended = false;
    Clock::duration m_decodeTime{0};

    // The sound.
    int m_audioIndex = -1;
    detail::CodecContextPtr m_audioCodec;
    detail::FramePtr m_audioFrame;
    detail::SwrPtr m_swr;
    AVRational m_audioTimeBase{0, 1};
    /// The video stream's start, seconds: the origin of the audio's timestamps too (the streams' common clock).
    double m_videoStartSeconds = 0.0;
    int m_swrFormat = -1;
    int m_swrRate = 0;
    int m_swrChannels = 0;
    int m_audioOutChannels = 2;
    bool m_audioWasEnabled = false;
    /// After a seek: audio frames ending before the target are dropped.
    std::optional<double> m_audioSeekTarget;
    /// The time of the next converted frame (the resampler's tail follows the last frame written).
    std::optional<double> m_audioNextPts;
    /// The picture's end (relative to the video stream's start) as the last pass that reached the end of the stream
    /// showed it: a longer sound is trimmed there.
    std::optional<double> m_videoEnd;
    std::vector<float> m_audioConverted;
    std::vector<float> m_audioStereo;
};

void VideoDecoder::Worker::run() noexcept {
    try {
        const std::wstring description = threadDescription(m_shared.settings.name);
        SetThreadDescription(GetCurrentThread(), description.c_str());
        if (open()) {
            decodeLoop();
        }
    } catch (const std::exception& e) {
        fail(DecodeFailure::DecoderError, fmt::format("unexpected exception: {}", e.what()));
    } catch (...) {
        fail(DecodeFailure::DecoderError, "unexpected exception");
    }
}

void VideoDecoder::Worker::fail(DecodeFailure failure, std::string message) {
    try {
        spdlog::warn("Video '{}' failed ({}): {}", m_shared.settings.name, toString(failure), message);
        const std::lock_guard lock(m_shared.infoMutex);
        if (!m_shared.error) {
            m_shared.error = Error{errorCodeFor(failure), std::move(message), core::pathToUtf8(m_shared.path)};
        }
    } catch (...) {
        // The state below is what the consumer relies on.
    }
    DecodeFailure none = DecodeFailure::None;
    m_shared.failure.compare_exchange_strong(none, failure);
    // Final, except after stop(): the thread may still be finishing when the owner stopped it, and Stopped stays.
    DecoderState current = m_shared.state.load();
    while (current != DecoderState::Stopped && !m_shared.state.compare_exchange_weak(current, DecoderState::Failed)) {
    }
}

bool VideoDecoder::Worker::open() {
    const DecoderSettings& settings = m_shared.settings;
    const std::filesystem::path& path = m_shared.path;
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    if (ec && status.type() != std::filesystem::file_type::not_found) {
        fail(DecodeFailure::ReadError, fmt::format("cannot look at the file: {}", ec.message()));
        return false;
    }
    if (!std::filesystem::exists(status)) {
        fail(DecodeFailure::FileNotFound, "no such file");
        return false;
    }
    if (!std::filesystem::is_regular_file(status)) {
        fail(DecodeFailure::FileNotFound, "not a regular file");
        return false;
    }
    if (const std::uintmax_t size = std::filesystem::file_size(path, ec); !ec && size == 0) {
        fail(DecodeFailure::InvalidFile, "the file is empty");
        return false;
    }

    AVFormatContext* input = avformat_alloc_context();
    if (input == nullptr) {
        fail(DecodeFailure::DecoderError, "out of memory (avformat_alloc_context)");
        return false;
    }
    input->interrupt_callback.callback = &Worker::interruptCallback;
    input->interrupt_callback.opaque = this;
    AVDictionary* options = nullptr;
    // A file's content must never make FFmpeg open anything but local files (playlists, references).
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    const std::string url = detail::fileUrl(path);
    int result = avformat_open_input(&input, url.c_str(), nullptr, &options); // frees `input` on failure
    av_dict_free(&options);
    if (result < 0) {
        if (m_stop.stop_requested()) {
            return false;
        }
        fail(result == AVERROR(ENOENT)   ? DecodeFailure::FileNotFound
             : isSystemReadError(result) ? DecodeFailure::ReadError
                                         : DecodeFailure::InvalidFile,
             fmt::format("cannot open the file: {}", detail::errorText(result)));
        return false;
    }
    m_format.reset(input);
    result = avformat_find_stream_info(m_format.get(), nullptr);
    if (result < 0) {
        if (m_stop.stop_requested()) {
            return false;
        }
        const int readError = systemReadError(result);
        fail(readError < 0 ? DecodeFailure::ReadError : DecodeFailure::InvalidFile,
             fmt::format("cannot read the streams: {}", detail::errorText(readError < 0 ? readError : result)));
        return false;
    }

    const AVCodec* decoder = nullptr;
    result = av_find_best_stream(m_format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (result >= 0 && (m_format->streams[result]->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0) {
        // Cover art of an audio file is not a video: look for a real video stream.
        result = AVERROR_STREAM_NOT_FOUND;
        for (unsigned index = 0; index < m_format->nb_streams; ++index) {
            const AVStream* stream = m_format->streams[index];
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
                decoder = avcodec_find_decoder(stream->codecpar->codec_id);
                result = decoder != nullptr ? static_cast<int>(index) : AVERROR_DECODER_NOT_FOUND;
                break;
            }
        }
    }
    if (result == AVERROR_DECODER_NOT_FOUND) {
        std::string codec = "unknown";
        for (unsigned index = 0; index < m_format->nb_streams; ++index) {
            if (m_format->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                const AVCodecParameters* parameters = m_format->streams[index]->codecpar;
                const char* name = avcodec_get_name(parameters->codec_id);
                std::array<char, 32> tag{};
                av_fourcc_make_string(tag.data(), parameters->codec_tag);
                codec = fmt::format("{} (tag {})", name != nullptr ? name : "none", tag.data());
                break;
            }
        }
        fail(DecodeFailure::UnsupportedCodec, fmt::format("no decoder for the video codec {} in this build", codec));
        return false;
    }
    if (result < 0) {
        fail(DecodeFailure::NoVideoStream, "the file holds no video stream");
        return false;
    }
    m_streamIndex = result;
    AVStream* stream = m_format->streams[m_streamIndex];

    m_codec.reset(avcodec_alloc_context3(decoder));
    m_frame.reset(av_frame_alloc());
    m_packet.reset(av_packet_alloc());
    if (!m_codec || !m_frame || !m_packet) {
        fail(DecodeFailure::DecoderError, "out of memory (decoder context)");
        return false;
    }
    result = avcodec_parameters_to_context(m_codec.get(), stream->codecpar);
    if (result < 0) {
        fail(DecodeFailure::DecoderError, fmt::format("cannot configure the decoder: {}", detail::errorText(result)));
        return false;
    }
    m_codec->thread_count = settings.threadCount;
    m_codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    m_codec->pkt_timebase = stream->time_base;
    result = avcodec_open2(m_codec.get(), decoder, nullptr);
    if (result < 0) {
        fail(result == AVERROR_PATCHWELCOME || result == AVERROR(ENOSYS) ? DecodeFailure::UnsupportedCodec
                                                                         : DecodeFailure::DecoderError,
             fmt::format("cannot open the {} decoder: {}", decoder->name, detail::errorText(result)));
        return false;
    }

    m_timeBase = stream->time_base;
    m_timestampsUsable = m_timeBase.num > 0 && m_timeBase.den > 0;
    m_startTimestamp = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    const double frameRate = rationalOrZero(av_guess_frame_rate(m_format.get(), stream, nullptr));
    m_nominalDuration = frameRate > 0.0 ? 1.0 / frameRate : kFallbackFrameSeconds;
    if (m_timestampsUsable && stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
        m_streamDuration = detail::toSeconds(stream->duration, m_timeBase);
    } else {
        m_streamDuration = taggedDurationSeconds(*stream);
    }
    m_declaredDuration = m_streamDuration;
    if (m_declaredDuration <= 0.0 && m_format->duration != AV_NOPTS_VALUE && m_format->duration > 0) {
        m_declaredDuration = static_cast<double>(m_format->duration) / static_cast<double>(AV_TIME_BASE);
    }
    const double sampleAspect = rationalOrZero(av_guess_sample_aspect_ratio(m_format.get(), stream, nullptr));
    m_streamSampleAspect = sampleAspect > 0.0 ? sampleAspect : 1.0;

    StreamInfo info;
    info.container = m_format->iformat != nullptr && m_format->iformat->name != nullptr ? m_format->iformat->name : "";
    info.codec = decoder->name != nullptr ? decoder->name : "";
    info.pixelFormat = pixelFormatName(stream->codecpar->format);
    info.codedSize = FrameSize{stream->codecpar->width, stream->codecpar->height};
    info.sampleAspect = m_streamSampleAspect;
    info.outputSize = scaledFrameSize(info.codedSize, m_streamSampleAspect, settings.maxHeight);
    info.frameRate = frameRate;
    info.durationSeconds = m_declaredDuration;
    info.streamDurationSeconds = m_streamDuration;
    info.bitRate = stream->codecpar->bit_rate > 0 ? stream->codecpar->bit_rate : m_format->bit_rate;
    info.decoderThreads = m_codec->thread_count;
    m_videoStartSeconds = m_timestampsUsable ? detail::toSeconds(m_startTimestamp, m_timeBase) : 0.0;
    openAudio(info);
    spdlog::info("Video '{}': {} {}x{} {} (sample aspect {:.3f}), {:.3f} fps, {:.2f} s, {} kb/s, container {}; frames "
                 "{}x{} RGBA, {} decoder thread(s), queue of {}",
                 settings.name, info.codec, info.codedSize.width, info.codedSize.height, info.pixelFormat,
                 info.sampleAspect, info.frameRate, info.durationSeconds, info.bitRate / 1000, info.container,
                 info.outputSize.width, info.outputSize.height, info.decoderThreads, settings.queueCapacity);
    {
        const std::lock_guard lock(m_shared.infoMutex);
        m_shared.info = std::move(info);
    }
    m_shared.openNanoseconds.store(nanosecondsSince(m_shared.started), std::memory_order_relaxed);
    m_shared.setState(DecoderState::Playing);
    resetPass(true);
    return true;
}

int VideoDecoder::Worker::systemReadError(int result) const noexcept {
    if (isSystemReadError(result)) {
        return result;
    }
    const int recorded = m_format && m_format->pb != nullptr ? m_format->pb->error : 0;
    return isSystemReadError(recorded) ? recorded : 0;
}

void VideoDecoder::Worker::resetPass(bool fromStart) noexcept {
    m_passStart.reset();
    m_passEnd = 0.0;
    m_passFrames = 0;
    m_passFromStart = fromStart;
    m_previousRelative.reset();
    m_previousDuration = m_nominalDuration;
    m_consecutiveErrors = 0;
    m_errorHistory = 0;
    m_decodedClean = false;
    m_truncatedRead = false;
}

std::optional<VideoDecoder::Worker::SeekRequest> VideoDecoder::Worker::takeSeekRequest() {
    const std::lock_guard lock(m_shared.controlMutex);
    if (!m_shared.seekRequest) {
        return std::nullopt;
    }
    // The generation of the latest seek, set with the request under the same lock.
    const SeekRequest request{*m_shared.seekRequest, m_shared.generation.load()};
    m_shared.seekRequest.reset();
    return request;
}

bool VideoDecoder::Worker::waitWhilePaused() {
    if (m_seekFramePending) {
        return true; // the paused picture follows a seek: its first frame is decoded before the pause holds
    }
    std::unique_lock lock(m_shared.controlMutex);
    m_shared.control.wait(lock, m_stop, [this] { return !m_shared.paused.load() || m_shared.seekRequest.has_value(); });
    return !m_stop.stop_requested();
}

bool VideoDecoder::Worker::waitAfterEnd() {
    std::unique_lock lock(m_shared.controlMutex);
    m_shared.control.wait(lock, m_stop, [this] { return m_shared.seekRequest.has_value(); });
    return !m_stop.stop_requested();
}

void VideoDecoder::Worker::decodeLoop() {
    while (!m_stop.stop_requested()) {
        if (!waitWhilePaused()) {
            return;
        }
        if (const std::optional<SeekRequest> request = takeSeekRequest()) {
            if (!applySeek(*request)) {
                return;
            }
            continue;
        }
        if (m_ended) {
            if (!waitAfterEnd()) {
                return;
            }
            continue;
        }
        if (!step()) {
            return;
        }
    }
}

bool VideoDecoder::Worker::step() {
    const int result = av_read_frame(m_format.get(), m_packet.get());
    if (result == AVERROR_EXIT || m_stop.stop_requested()) {
        av_packet_unref(m_packet.get());
        return false;
    }
    if (result < 0 && result != AVERROR(EAGAIN)) {
        // The system failed a read (the file's own end is no error): no fault of the clip, and no end of its stream (a
        // failed read also sets the end-of-file flag, which would read as a file cut short).
        if (const int readError = systemReadError(result); readError < 0) {
            fail(DecodeFailure::ReadError, fmt::format("cannot read the file: {}", detail::errorText(readError)));
            return false;
        }
    }
    if (result == AVERROR_EOF) {
        return endOfStream();
    }
    if (result == AVERROR(EAGAIN)) {
        return true;
    }
    if (result < 0) {
        if (m_format->pb != nullptr && avio_feof(m_format->pb) != 0) {
            // The demuxer expected more data than the file holds ("partial file"): the file was cut short. Show what
            // was decoded, then fail.
            m_truncatedRead = true;
            spdlog::debug("Video '{}': the file ends mid-stream ({})", m_shared.settings.name,
                          detail::errorText(result));
            return endOfStream();
        }
        return decodeError(result, "read");
    }
    m_shared.packetsRead.fetch_add(1, std::memory_order_relaxed);
    if (m_packet->stream_index == m_audioIndex) {
        audioPacket(*m_packet);
        av_packet_unref(m_packet.get());
        return true;
    }
    if (m_packet->stream_index != m_streamIndex) {
        av_packet_unref(m_packet.get());
        return true;
    }
    if ((m_packet->flags & AV_PKT_FLAG_CORRUPT) != 0) {
        m_shared.corruptPackets.fetch_add(1, std::memory_order_relaxed);
    }
    const bool decoded = sendPacket(m_packet.get());
    av_packet_unref(m_packet.get());
    return decoded;
}

bool VideoDecoder::Worker::decodeError(int code, std::string_view what) {
    m_shared.decodeErrors.fetch_add(1, std::memory_order_relaxed);
    return noteError(fmt::format("{} failed: {}", what, detail::errorText(code)));
}

bool VideoDecoder::Worker::noteError(std::string_view what) {
    const DecoderSettings& settings = m_shared.settings;
    m_errorHistory = (m_errorHistory << 1) | 1u;
    if (++m_consecutiveErrors >= settings.maxConsecutiveErrors) {
        fail(DecodeFailure::CorruptData, fmt::format("{} errors in a row, the last: {}", m_consecutiveErrors, what));
        return false;
    }
    const std::uint64_t window =
        settings.corruptErrorWindow >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << settings.corruptErrorWindow) - 1;
    const int recent = std::popcount(m_errorHistory & window);
    if (m_decodedClean && recent >= settings.corruptErrorThreshold) {
        fail(DecodeFailure::CorruptData, fmt::format("{} errors among the last {} decoding events, the last: {}",
                                                     recent, settings.corruptErrorWindow, what));
        return false;
    }
    return true;
}

bool VideoDecoder::Worker::sendPacket(const AVPacket* packet) {
    for (;;) {
        const Clock::time_point start = Clock::now();
        const int result = avcodec_send_packet(m_codec.get(), packet);
        m_decodeTime += Clock::now() - start;
        if (result == AVERROR(EAGAIN)) {
            // Output must be read before more input is accepted; then the same packet is sent again.
            if (!receiveFrames()) {
                return false;
            }
            continue;
        }
        if (result == AVERROR_EOF) {
            return receiveFrames(); // already draining
        }
        if (result < 0) {
            return decodeError(result, "decode");
        }
        return receiveFrames();
    }
}

bool VideoDecoder::Worker::receiveFrames() {
    for (;;) {
        const Clock::time_point start = Clock::now();
        const int result = avcodec_receive_frame(m_codec.get(), m_frame.get());
        m_decodeTime += Clock::now() - start;
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return true;
        }
        if (result < 0) {
            if (!decodeError(result, "decode")) {
                return false;
            }
            continue;
        }
        m_shared.framesDecoded.fetch_add(1, std::memory_order_relaxed);
        if (m_frame->decode_error_flags != 0 || (m_frame->flags & AV_FRAME_FLAG_CORRUPT) != 0) {
            // Decoded with concealed blocks or missing references: shown, but it counts as an error.
            m_shared.damagedFrames.fetch_add(1, std::memory_order_relaxed);
            if (!noteError(fmt::format("frame decoded with errors (flags {:#x})", m_frame->decode_error_flags))) {
                av_frame_unref(m_frame.get());
                return false;
            }
        } else {
            m_consecutiveErrors = 0;
            m_decodedClean = true;
            m_errorHistory <<= 1;
        }
        const bool queued = emitFrame();
        av_frame_unref(m_frame.get());
        if (!queued) {
            return false;
        }
    }
}

bool VideoDecoder::Worker::ensureConverter(const AVFrame& frame) {
    const FrameSize source{frame.width, frame.height};
    const double sampleAspect =
        frame.sample_aspect_ratio.num > 0 ? rationalOrZero(frame.sample_aspect_ratio) : m_streamSampleAspect;
    const int colorspace = swsColorspace(frame.colorspace, frame.height);
    const int range = frame.color_range == AVCOL_RANGE_JPEG || isFullRangeFormat(frame.format) ? 1 : 0;
    if (m_sws && source == m_sourceSize && frame.format == m_sourceFormat && colorspace == m_sourceColorspace &&
        range == m_sourceRange && sampleAspect == m_sourceSampleAspect) {
        return true;
    }
    const FrameSize output = scaledFrameSize(source, sampleAspect, m_shared.settings.maxHeight);
    if (!output.valid()) {
        return false;
    }
    m_sws.reset(sws_getContext(source.width, source.height, static_cast<AVPixelFormat>(frame.format), output.width,
                               output.height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!m_sws) {
        return false;
    }
    // Decoded YUV to full-range RGB with the stream's matrix. Fails harmlessly for RGB sources.
    sws_setColorspaceDetails(m_sws.get(), sws_getCoefficients(colorspace), range, sws_getCoefficients(SWS_CS_DEFAULT),
                             1, 0, 1 << 16, 1 << 16);
    if (m_sourceSize.valid() && (source != m_sourceSize || output != m_outputSize)) {
        spdlog::info("Video '{}': frames change from {}x{} to {}x{} (delivered at {}x{})", m_shared.settings.name,
                     m_sourceSize.width, m_sourceSize.height, source.width, source.height, output.width, output.height);
    }
    m_sourceSize = source;
    m_sourceFormat = frame.format;
    m_sourceColorspace = colorspace;
    m_sourceRange = range;
    m_sourceSampleAspect = sampleAspect;
    m_outputSize = output;
    return true;
}

bool VideoDecoder::Worker::emitFrame() {
    if (seekPending()) {
        // A newer seek waits: this frame belongs to the position it leaves. decodeLoop() carries the seek out next.
        m_decodeTime = Clock::duration::zero();
        return true;
    }
    const AVFrame& frame = *m_frame;
    const double duration =
        m_timestampsUsable && frame.duration > 0 ? detail::toSeconds(frame.duration, m_timeBase) : m_nominalDuration;
    std::int64_t timestamp = frame.best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame.pts;
    }
    double relative = 0.0;
    if (m_timestampsUsable && timestamp != AV_NOPTS_VALUE) {
        relative = detail::toSeconds(timestamp - m_startTimestamp, m_timeBase);
    } else if (m_previousRelative) {
        relative = *m_previousRelative + m_previousDuration; // no timestamp: the frame follows the previous one
    }
    m_previousRelative = relative;
    m_previousDuration = duration;

    if (m_seekTarget) {
        if (relative + duration <= *m_seekTarget + 1e-6) {
            // Decoded from the key frame before the target: not shown.
            m_shared.framesSkipped.fetch_add(1, std::memory_order_relaxed);
            m_decodeTime = Clock::duration::zero();
            return true;
        }
        m_seekTarget.reset();
    }

    m_passStart = m_passStart ? std::min(*m_passStart, relative) : relative;
    m_passEnd = std::max(m_passEnd, relative + duration);
    ++m_passFrames;
    double pts = m_loopOffset + relative;
    if (pts <= m_lastPts) {
        pts = m_lastPts + kTimestampNudgeSeconds;
    }
    m_lastPts = pts;

    const Clock::time_point acquireStart = Clock::now();
    PooledFrame out = m_shared.pool->acquire(m_stop);
    if (!out) {
        return false;
    }
    Clock::duration waited = Clock::now() - acquireStart;
    if (!ensureConverter(frame)) {
        fail(DecodeFailure::DecoderError, fmt::format("cannot convert {}x{} {} frames to RGBA", frame.width,
                                                      frame.height, pixelFormatName(frame.format)));
        return false;
    }
    if (out->resize(m_outputSize)) {
        m_shared.frameAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    const Clock::time_point convertStart = Clock::now();
    std::array<std::uint8_t*, 4> planes{out->pixels(), nullptr, nullptr, nullptr};
    const std::array<int, 4> strides{static_cast<int>(out->strideBytes()), 0, 0, 0};
    const int rows = sws_scale(m_sws.get(), frame.data, frame.linesize, 0, frame.height, planes.data(), strides.data());
    const Clock::duration convertTime = Clock::now() - convertStart;
    if (rows <= 0) {
        fail(DecodeFailure::DecoderError, fmt::format("sws_scale failed: {}", detail::errorText(rows)));
        return false;
    }
    {
        const std::lock_guard lock(m_shared.timingMutex);
        m_shared.decodeTimes.record(milliseconds(m_decodeTime));
        m_shared.convertTimes.record(milliseconds(convertTime));
    }
    m_decodeTime = Clock::duration::zero();

    out->timing = FrameTiming{pts, duration, m_generation};
    out->serial = m_serial++;
    out->loop = m_loop;
    const Clock::time_point pushStart = Clock::now();
    const PushResult pushed = m_shared.queue.push(std::move(out), m_stop);
    waited += Clock::now() - pushStart;
    m_shared.queueWaitNanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count(),
                                            std::memory_order_relaxed);
    switch (pushed) {
    case PushResult::Queued:
        m_shared.framesQueued.fetch_add(1, std::memory_order_relaxed);
        if (m_shared.firstFrameNanoseconds.load(std::memory_order_relaxed) < 0) {
            m_shared.firstFrameNanoseconds.store(nanosecondsSince(m_shared.started), std::memory_order_relaxed);
        }
        m_seekFramePending = false;
        return true;
    case PushResult::Discarded:
        return true; // a seek came while the frame waited for room: decodeLoop() carries it out next
    case PushResult::Refused:
        break;
    }
    return false;
}

bool VideoDecoder::Worker::endOfStream() {
    // Drain: the decoder returns the frames it still holds (B-frame reordering, frame threads).
    if (!sendPacket(nullptr)) {
        return false;
    }
    if (m_passFrames > 0) {
        m_videoEnd = m_passEnd; // every frame of the pass shown: the picture ends here, on every pass
    }
    // Then the sound's last frames, up to the picture's end.
    drainAudio();
    return endOfPass();
}

bool VideoDecoder::Worker::endOfPass() {
    const std::string& name = m_shared.settings.name;
    // A seek past the last frame ends the pass without reaching its target: the next pass shows every frame.
    m_seekTarget.reset();
    if (m_truncatedRead) {
        fail(DecodeFailure::Truncated, fmt::format("the file ends mid-stream after {:.2f} s of {:.2f} s",
                                                   m_passFrames > 0 ? m_passEnd : 0.0, m_declaredDuration));
        return false;
    }
    if (m_passFrames == 0 && m_passFromStart) {
        fail(DecodeFailure::CorruptData, "no frame of the stream could be decoded");
        return false;
    }
    // Only the video stream's own duration tells a cut: the container's covers every stream (Matroska and WebM give
    // no other when their tracks have no DURATION tag), and an audio track may outlast the video.
    if (m_firstPass && m_passFromStart && m_streamDuration > 0.0) {
        const double span = m_passEnd - m_passStart.value_or(0.0);
        if (span < m_streamDuration * kTruncationRatio - kTruncationSlackSeconds) {
            fail(DecodeFailure::Truncated,
                 fmt::format("the stream ends after {:.2f} s of the {:.2f} s it declares", span, m_streamDuration));
            return false;
        }
    }
    if (!m_shared.settings.loop) {
        m_ended = true;
        m_seekFramePending = false; // a seek past the end: no frame comes
        m_shared.setState(DecoderState::Ended);
        spdlog::info("Video '{}': end of stream after {} frame(s)", name, m_serial);
        return true;
    }
    m_loopOffset += std::max(m_passEnd, m_nominalDuration);
    ++m_loop;
    m_firstPass = false;
    m_shared.loops.fetch_add(1, std::memory_order_relaxed);
    if (!seekTo(0.0)) {
        return false;
    }
    resetAudio();
    resetPass(true);
    return true;
}

bool VideoDecoder::Worker::seekTo(double seconds) {
    const std::int64_t target =
        m_startTimestamp +
        (m_timestampsUsable ? static_cast<std::int64_t>(std::llround(seconds / av_q2d(m_timeBase))) : std::int64_t{0});
    int result = av_seek_frame(m_format.get(), m_streamIndex, target, AVSEEK_FLAG_BACKWARD);
    if (result < 0) {
        result = avformat_seek_file(m_format.get(), m_streamIndex, std::numeric_limits<std::int64_t>::min(), target,
                                    target, 0);
    }
    if (result < 0) {
        if (!m_stop.stop_requested()) {
            const int readError = systemReadError(result);
            fail(readError < 0 ? DecodeFailure::ReadError : DecodeFailure::DecoderError,
                 fmt::format("cannot seek to {:.3f} s: {}", seconds,
                             detail::errorText(readError < 0 ? readError : result)));
        }
        return false;
    }
    avcodec_flush_buffers(m_codec.get());
    m_decodeTime = Clock::duration::zero();
    return true;
}

bool VideoDecoder::Worker::applySeek(const SeekRequest& request) {
    // seek() already discarded the queued frames of the old position, on the consumer's thread: the thread only
    // appends to the queue, so the consumer never sees frames vanish while it reads them.
    const double target = std::max(0.0, request.seconds);
    m_generation = request.generation;
    m_shared.seeks.fetch_add(1, std::memory_order_relaxed);
    if (!seekTo(std::max(0.0, target - kSeekPrerollSeconds))) {
        return false;
    }
    // A new generation: timestamps start again at the target, without the loop offset.
    m_loopOffset = 0.0;
    m_lastPts = -std::numeric_limits<double>::infinity();
    m_seekTarget = target;
    resetAudio();
    m_audioSeekTarget = target;
    m_ended = false;
    m_seekFramePending = true;
    resetPass(target <= 0.0);
    m_firstPass = false;
    m_shared.setState(DecoderState::Playing);
    spdlog::debug("Video '{}': seek to {:.3f} s (generation {})", m_shared.settings.name, target, m_generation);
    return true;
}

// --- The sound
// ---------------------------------------------------------------------------------------------------------

void VideoDecoder::Worker::openAudio(StreamInfo& info) {
    const AVCodec* decoder = nullptr;
    const int index = av_find_best_stream(m_format.get(), AVMEDIA_TYPE_AUDIO, -1, m_streamIndex, &decoder, 0);
    if (index < 0) {
        return;
    }
    const AVStream* stream = m_format->streams[index];
    const AVCodecParameters& parameters = *stream->codecpar;
    const char* codecName = avcodec_get_name(parameters.codec_id);
    info.audioCodec = codecName != nullptr ? codecName : "";
    info.audioSampleRate = parameters.sample_rate;
    info.audioChannels = parameters.ch_layout.nb_channels;
    m_audioIndex = index; // its packets are discarded without a sink
    if (!m_shared.settings.audioSink) {
        return;
    }
    if (decoder == nullptr) {
        spdlog::info("Video '{}': no decoder for the {} sound in this build; the clip plays silent",
                     m_shared.settings.name, info.audioCodec);
        return;
    }
    detail::CodecContextPtr codec(avcodec_alloc_context3(decoder));
    detail::FramePtr frame(av_frame_alloc());
    if (!codec || !frame || avcodec_parameters_to_context(codec.get(), &parameters) < 0) {
        spdlog::info("Video '{}': the sound's decoder cannot be set up; the clip plays silent", m_shared.settings.name);
        return;
    }
    codec->thread_count = 1;
    codec->pkt_timebase = stream->time_base;
    if (const int result = avcodec_open2(codec.get(), decoder, nullptr); result < 0) {
        spdlog::info("Video '{}': the {} sound cannot be decoded ({}); the clip plays silent", m_shared.settings.name,
                     decoder->name, detail::errorText(result));
        return;
    }
    m_audioCodec = std::move(codec);
    m_audioFrame = std::move(frame);
    m_audioTimeBase = stream->time_base;
    info.audioDecoded = true;
    spdlog::info("Video '{}': sound {} {} Hz, {} channel(s), resampled to {} Hz stereo", m_shared.settings.name,
                 info.audioCodec, info.audioSampleRate, info.audioChannels, m_shared.settings.audioSampleRate);
}

void VideoDecoder::Worker::resetAudio() noexcept {
    if (m_audioCodec) {
        avcodec_flush_buffers(m_audioCodec.get());
    }
    m_swr.reset();
    m_swrFormat = -1;
    m_audioSeekTarget.reset();
    m_audioNextPts.reset();
}

std::optional<double> VideoDecoder::Worker::audioEndSeconds() const noexcept {
    if (m_videoEnd) {
        return m_videoEnd;
    }
    if (m_streamDuration > 0.0) {
        return std::max(m_streamDuration, m_passEnd);
    }
    return std::nullopt; // the first pass of a stream without a duration: known at its end only
}

void VideoDecoder::Worker::drainAudio() {
    if (!m_audioCodec || !m_audioWasEnabled || !m_shared.audioEnabled.load(std::memory_order_relaxed) ||
        seekPending()) {
        return;
    }
    if (avcodec_send_packet(m_audioCodec.get(), nullptr) >= 0) {
        for (;;) {
            const int result = avcodec_receive_frame(m_audioCodec.get(), m_audioFrame.get());
            if (result < 0) {
                break; // AVERROR_EOF: drained
            }
            emitAudio(*m_audioFrame);
            av_frame_unref(m_audioFrame.get());
        }
    }
    if (!m_swr || !m_audioNextPts) {
        return;
    }
    const int capacity = swr_get_out_samples(m_swr.get(), 0);
    if (capacity <= 0) {
        return;
    }
    m_audioConverted.resize(static_cast<std::size_t>(capacity) * static_cast<std::size_t>(m_audioOutChannels));
    std::uint8_t* out = reinterpret_cast<std::uint8_t*>(m_audioConverted.data());
    const int flushed = swr_convert(m_swr.get(), &out, capacity, nullptr, 0);
    if (flushed > 0) {
        writeAudio(m_audioConverted.data(), static_cast<std::size_t>(flushed), *m_audioNextPts);
    }
}

void VideoDecoder::Worker::audioPacket(const AVPacket& packet) {
    const bool enabled = m_shared.audioEnabled.load(std::memory_order_relaxed) && m_audioCodec;
    if (!enabled) {
        m_audioWasEnabled = false;
        m_shared.audioPacketsDiscarded.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!m_audioWasEnabled) {
        resetAudio(); // the sound starts again here: nothing of before is kept
        m_audioWasEnabled = true;
    }
    if (seekPending()) {
        return; // of the position a newer seek leaves
    }
    int result = avcodec_send_packet(m_audioCodec.get(), &packet);
    if (result < 0 && result != AVERROR(EAGAIN)) {
        m_shared.audioDecodeErrors.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (;;) {
        result = avcodec_receive_frame(m_audioCodec.get(), m_audioFrame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            return;
        }
        if (result < 0) {
            m_shared.audioDecodeErrors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        emitAudio(*m_audioFrame);
        av_frame_unref(m_audioFrame.get());
    }
}

void VideoDecoder::Worker::emitAudio(const AVFrame& frame) {
    const int inRate = frame.sample_rate;
    const int inChannels = frame.ch_layout.nb_channels;
    if (inRate <= 0 || inChannels <= 0 || frame.nb_samples <= 0) {
        return;
    }
    std::int64_t timestamp = frame.best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = frame.pts;
    }
    if (timestamp == AV_NOPTS_VALUE || m_audioTimeBase.num <= 0 || m_audioTimeBase.den <= 0) {
        return; // no time: it could not follow the picture
    }
    // The streams share the file's clock: the audio's time from the video stream's start, plus the loops.
    const double pts = m_loopOffset + detail::toSeconds(timestamp, m_audioTimeBase) - m_videoStartSeconds;
    const double duration = static_cast<double>(frame.nb_samples) / static_cast<double>(inRate);
    if (m_audioSeekTarget) {
        if (pts + duration <= *m_audioSeekTarget) {
            return; // decoded from before the seek's target
        }
        m_audioSeekTarget.reset();
    }
    const int outRate = m_shared.settings.audioSampleRate;
    if (!m_swr || frame.format != m_swrFormat || inRate != m_swrRate || inChannels != m_swrChannels) {
        AVChannelLayout inLayout{};
        if (frame.ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            av_channel_layout_default(&inLayout, inChannels);
        } else if (av_channel_layout_copy(&inLayout, &frame.ch_layout) < 0) {
            return;
        }
        // Mono stays mono (copied to both channels below, at its level); anything else becomes stereo.
        m_audioOutChannels = inChannels == 1 ? 1 : 2;
        AVChannelLayout outLayout{};
        av_channel_layout_default(&outLayout, m_audioOutChannels);
        SwrContext* raw = nullptr;
        int result = swr_alloc_set_opts2(&raw, &outLayout, AV_SAMPLE_FMT_FLT, outRate, &inLayout,
                                         static_cast<AVSampleFormat>(frame.format), inRate, 0, nullptr);
        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);
        detail::SwrPtr swr(raw);
        if (result >= 0) {
            result = swr_init(swr.get());
        }
        if (result < 0) {
            m_shared.audioDecodeErrors.fetch_add(1, std::memory_order_relaxed);
            spdlog::debug("Video '{}': the sound cannot be resampled: {}", m_shared.settings.name,
                          detail::errorText(result));
            return;
        }
        m_swr = std::move(swr);
        m_swrFormat = frame.format;
        m_swrRate = inRate;
        m_swrChannels = inChannels;
    }
    // The resampler holds a few samples of its filter: the first sample out was put in that much earlier, so its time
    // is the frame's less that delay.
    const double delay = static_cast<double>(swr_get_delay(m_swr.get(), outRate)) / static_cast<double>(outRate);
    const int capacity = swr_get_out_samples(m_swr.get(), frame.nb_samples);
    if (capacity <= 0) {
        return;
    }
    m_audioConverted.resize(static_cast<std::size_t>(capacity) * static_cast<std::size_t>(m_audioOutChannels));
    std::uint8_t* out = reinterpret_cast<std::uint8_t*>(m_audioConverted.data());
    const int converted = swr_convert(m_swr.get(), &out, capacity,
                                      const_cast<const std::uint8_t**>(frame.extended_data), frame.nb_samples);
    if (converted <= 0) {
        if (converted < 0) {
            m_shared.audioDecodeErrors.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    writeAudio(m_audioConverted.data(), static_cast<std::size_t>(converted), pts - delay);
}

void VideoDecoder::Worker::writeAudio(const float* converted, std::size_t frames, double ptsSeconds) {
    const double rate = static_cast<double>(m_shared.settings.audioSampleRate);
    m_audioNextPts = ptsSeconds + static_cast<double>(frames) / rate;
    // A sound longer than its picture stops at the picture's end: the next loop's sound starts there.
    if (const std::optional<double> end = audioEndSeconds()) {
        const double room = (m_loopOffset + *end - ptsSeconds) * rate;
        const double kept = std::floor(room + 1e-6);
        if (kept <= 0.0) {
            return;
        }
        frames = std::min(frames, static_cast<std::size_t>(kept));
    }
    const float* stereo = converted;
    if (m_audioOutChannels == 1) {
        m_audioStereo.resize(frames * 2);
        for (std::size_t index = 0; index < frames; ++index) {
            m_audioStereo[index * 2] = converted[index];
            m_audioStereo[index * 2 + 1] = converted[index];
        }
        stereo = m_audioStereo.data();
    }
    m_shared.settings.audioSink->write(std::span<const float>(stereo, frames * 2), ptsSeconds, m_generation);
    m_shared.audioFramesWritten.fetch_add(frames, std::memory_order_relaxed);
}

// --- VideoDecoder
// -------------------------------------------------------------------------------------------------------

void VideoDecoder::HandleCloser::operator()(void* handle) const noexcept {
    if (handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle));
    }
}

VideoDecoder::VideoDecoder(std::shared_ptr<Shared> shared) noexcept : m_shared(std::move(shared)) {}

Expected<std::unique_ptr<VideoDecoder>> VideoDecoder::start(std::filesystem::path path, DecoderSettings settings) {
    const std::string context = core::pathToUtf8(path);
    if (path.empty()) {
        return makeError(ErrorCode::InvalidArgument, "no video file given");
    }
    if (settings.maxHeight < 0) {
        return makeError(ErrorCode::InvalidArgument, fmt::format("maxHeight {} is negative", settings.maxHeight),
                         context);
    }
    if (settings.queueCapacity < 1 || settings.queueCapacity > DecoderSettings::kMaxQueueCapacity) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("queueCapacity {} is outside [1, {}]", settings.queueCapacity,
                                     DecoderSettings::kMaxQueueCapacity),
                         context);
    }
    if (settings.threadCount < 1 || settings.threadCount > DecoderSettings::kMaxThreadCount) {
        return makeError(
            ErrorCode::InvalidArgument,
            fmt::format("threadCount {} is outside [1, {}]", settings.threadCount, DecoderSettings::kMaxThreadCount),
            context);
    }
    if (settings.maxConsecutiveErrors < 1) {
        return makeError(ErrorCode::InvalidArgument, "maxConsecutiveErrors must be at least 1", context);
    }
    if (settings.corruptErrorWindow < 1 || settings.corruptErrorWindow > 64 || settings.corruptErrorThreshold < 1) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("corruptErrorWindow {} must be within [1, 64] and corruptErrorThreshold {} at "
                                     "least 1",
                                     settings.corruptErrorWindow, settings.corruptErrorThreshold),
                         context);
    }
    if (settings.audioSampleRate < 8000 || settings.audioSampleRate > 192000) {
        return makeError(ErrorCode::InvalidArgument,
                         fmt::format("audioSampleRate {} is outside [8000, 192000]", settings.audioSampleRate), context);
    }
    if (settings.name.empty()) {
        settings.name = core::pathToUtf8(path.filename());
    }
    detail::installLogBridge();

    auto shared = std::make_shared<Shared>(std::move(path), std::move(settings));
    shared->paused.store(shared->settings.startPaused);
    shared->audioEnabled.store(shared->settings.audioEnabled);
    std::unique_ptr<VideoDecoder> decoder(new VideoDecoder(shared));
    try {
        decoder->m_thread = std::jthread([shared](std::stop_token stop) { Worker(*shared, std::move(stop)).run(); });
    } catch (const std::system_error& e) {
        return makeError(ErrorCode::Unknown, fmt::format("cannot start the decoder thread: {}", e.what()), context);
    }
    // A handle of the decoder's own: the jthread goes to stoppedThreads() on stop(), and its handle with it.
    HANDLE duplicate = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(decoder->m_thread.native_handle()),
                        GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS) != 0) {
        decoder->m_threadHandle.reset(duplicate);
    }
    return decoder;
}

VideoDecoder::~VideoDecoder() {
    stop();
}

void VideoDecoder::stop() {
    if (m_thread.joinable()) {
        m_thread.request_stop(); // wakes every wait of the thread and interrupts FFmpeg's I/O between reads
        m_shared->queue.close();
        m_shared->pool->close();
        try {
            // Never joined here: the thread may be in a read of a slow disk or network share (spec §9.4).
            stoppedThreads().adopt(std::move(m_thread));
        } catch (...) {
            // Out of memory to keep it: the thread was joined when adopt()'s argument was destroyed.
        }
    }
    m_shared->setState(DecoderState::Stopped);
}

FrameQueue& VideoDecoder::frames() noexcept {
    return m_shared->queue;
}

const FrameQueue& VideoDecoder::frames() const noexcept {
    return m_shared->queue;
}

DecoderState VideoDecoder::state() const noexcept {
    const DecoderState state = m_shared->state.load(std::memory_order_acquire);
    if (state == DecoderState::Playing && m_shared->paused.load(std::memory_order_acquire)) {
        return DecoderState::Paused;
    }
    return state;
}

DecodeFailure VideoDecoder::failure() const noexcept {
    return m_shared->failure.load(std::memory_order_acquire);
}

std::uint32_t VideoDecoder::generation() const noexcept {
    return m_shared->generation.load(std::memory_order_acquire);
}

std::optional<Error> VideoDecoder::error() const {
    const std::lock_guard lock(m_shared->infoMutex);
    return m_shared->error;
}

std::optional<StreamInfo> VideoDecoder::streamInfo() const {
    const std::lock_guard lock(m_shared->infoMutex);
    return m_shared->info;
}

DecoderStats VideoDecoder::stats() const {
    const Shared& shared = *m_shared;
    const auto toMilliseconds = [](std::int64_t nanoseconds) {
        return nanoseconds >= 0 ? static_cast<double>(nanoseconds) * 1e-6 : -1.0;
    };
    DecoderStats stats;
    stats.packetsRead = shared.packetsRead.load(std::memory_order_relaxed);
    stats.framesDecoded = shared.framesDecoded.load(std::memory_order_relaxed);
    stats.framesQueued = shared.framesQueued.load(std::memory_order_relaxed);
    stats.framesSkipped = shared.framesSkipped.load(std::memory_order_relaxed);
    stats.decodeErrors = shared.decodeErrors.load(std::memory_order_relaxed);
    stats.damagedFrames = shared.damagedFrames.load(std::memory_order_relaxed);
    stats.corruptPackets = shared.corruptPackets.load(std::memory_order_relaxed);
    stats.loops = shared.loops.load(std::memory_order_relaxed);
    stats.seeks = shared.seeks.load(std::memory_order_relaxed);
    stats.frameAllocations = shared.frameAllocations.load(std::memory_order_relaxed);
    stats.queueWaitSeconds = static_cast<double>(shared.queueWaitNanoseconds.load(std::memory_order_relaxed)) * 1e-9;
    stats.openMilliseconds = toMilliseconds(shared.openNanoseconds.load(std::memory_order_relaxed));
    stats.firstFrameMilliseconds = toMilliseconds(shared.firstFrameNanoseconds.load(std::memory_order_relaxed));
    stats.audioFramesWritten = shared.audioFramesWritten.load(std::memory_order_relaxed);
    stats.audioPacketsDiscarded = shared.audioPacketsDiscarded.load(std::memory_order_relaxed);
    stats.audioDecodeErrors = shared.audioDecodeErrors.load(std::memory_order_relaxed);
    const std::lock_guard lock(shared.timingMutex);
    stats.decodeMilliseconds = shared.decodeTimes.summary();
    stats.convertMilliseconds = shared.convertTimes.summary();
    return stats;
}

double VideoDecoder::threadCpuSeconds() const {
    return m_threadHandle ? threadCpuSecondsOf(static_cast<HANDLE>(m_threadHandle.get())) : 0.0;
}

bool VideoDecoder::threadRunning() const noexcept {
    return m_threadHandle && WaitForSingleObject(static_cast<HANDLE>(m_threadHandle.get()), 0) == WAIT_TIMEOUT;
}

const std::filesystem::path& VideoDecoder::path() const noexcept {
    return m_shared->path;
}

const std::string& VideoDecoder::name() const noexcept {
    return m_shared->settings.name;
}

const DecoderSettings& VideoDecoder::settings() const noexcept {
    return m_shared->settings;
}

void VideoDecoder::pause() {
    const std::lock_guard lock(m_shared->controlMutex);
    m_shared->paused.store(true);
}

void VideoDecoder::setAudioEnabled(bool enabled) noexcept {
    m_shared->audioEnabled.store(enabled, std::memory_order_relaxed);
}

bool VideoDecoder::audioEnabled() const noexcept {
    return m_shared->audioEnabled.load(std::memory_order_relaxed);
}

void VideoDecoder::resume() {
    {
        const std::lock_guard lock(m_shared->controlMutex);
        m_shared->paused.store(false);
    }
    m_shared->control.notify_all();
}

std::uint32_t VideoDecoder::seek(double seconds) {
    std::uint32_t generation = 0;
    {
        const std::lock_guard lock(m_shared->controlMutex);
        m_shared->seekRequest = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
        generation = m_shared->generation.load() + 1;
        m_shared->generation.store(generation, std::memory_order_release);
        // An ended stream plays again from now on (the thread takes the request at once).
        DecoderState ended = DecoderState::Ended;
        m_shared->state.compare_exchange_strong(ended, DecoderState::Playing);
    }
    // On the consumer's thread, which alone removes frames: the frames of the old position go back to the pool, and a
    // thread waiting for room in the queue wakes and discards the frame it holds.
    m_shared->queue.discardOlderThan(generation);
    m_shared->control.notify_all();
    return generation;
}

namespace {

/// waitForStoppedDecoders()'s calls (stoppedDecoderWaits()).
std::atomic<std::uint64_t> g_stoppedDecoderWaits{0};

} // namespace

void waitForStoppedDecoders() {
    g_stoppedDecoderWaits.fetch_add(1, std::memory_order_relaxed);
    stoppedThreads().joinAll();
}

std::uint64_t stoppedDecoderWaits() noexcept {
    return g_stoppedDecoderWaits.load(std::memory_order_relaxed);
}

Expected<StreamInfo> probeVideo(const std::filesystem::path& path, int maxHeight) {
    detail::installLogBridge();
    DecoderSettings settings;
    settings.maxHeight = std::max(0, maxHeight);
    settings.name = core::pathToUtf8(path.filename());
    settings.timingCapacity = 0;
    VideoDecoder::Shared shared(path, std::move(settings));
    VideoDecoder::Worker worker(shared, std::stop_token{});
    if (!worker.open()) {
        const std::lock_guard lock(shared.infoMutex);
        if (shared.error) {
            return propagate(*shared.error);
        }
        return makeError(ErrorCode::Unknown, "the video could not be opened", core::pathToUtf8(path));
    }
    const std::lock_guard lock(shared.infoMutex);
    return *shared.info;
}

} // namespace rv::media
