#include <rv/media/video_decoder.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <windows.h>

#include <rv/core/text.hpp>
#include <rv/media/output_size.hpp>

#include "ffmpeg_support.hpp"

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
        return ErrorCode::IoError;
    }
    return ErrorCode::Unknown;
}

namespace {

using Clock = std::chrono::steady_clock;

/// A pass shorter than this share of the declared duration, less kTruncationSlackSeconds, means the file was cut short.
constexpr double kTruncationRatio = 0.9;
constexpr double kTruncationSlackSeconds = 0.5;
/// Frame duration when neither the frame nor the stream tells one.
constexpr double kFallbackFrameSeconds = 1.0 / 30.0;
/// Smallest step between two frames whose timestamps do not increase (broken files).
constexpr double kTimestampNudgeSeconds = 0.001;

double milliseconds(Clock::duration duration) noexcept {
    return std::chrono::duration<double, std::milli>(duration).count();
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

std::wstring threadDescription(const std::string& name) {
    return core::pathFromUtf8("rv video: " + name).wstring();
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

    std::atomic<DecoderState> state{DecoderState::Opening};
    std::atomic<DecodeFailure> failure{DecodeFailure::None};

    mutable std::mutex infoMutex;
    std::optional<StreamInfo> info;
    std::optional<Error> error;

    std::mutex controlMutex;
    std::condition_variable_any control;
    bool paused = false;
    std::optional<double> seekRequest;

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
    /// CPU time of the thread when it ended; negative while it runs.
    std::atomic<double> finalCpuSeconds{-1.0};

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
    [[nodiscard]] bool applySeek(double seconds);
    /// Counts an FFmpeg error (decodeErrors) and notes it (noteError).
    [[nodiscard]] bool decodeError(int code, std::string_view what);
    /// Notes an error or a damaged frame; false (and the decoder fails as CorruptData) when too many came in a row or
    /// within the corruption window.
    [[nodiscard]] bool noteError(std::string_view what);
    /// Waits while paused; processes nothing else. False on stop.
    [[nodiscard]] bool waitWhilePaused();
    /// Waits at the end of a stream that does not loop, until a seek or the stop. False on stop.
    [[nodiscard]] bool waitAfterEnd();
    [[nodiscard]] std::optional<double> takeSeekRequest();
    [[nodiscard]] bool ensureConverter(const AVFrame& frame);
    void resetPass(bool fromStart) noexcept;
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
    m_shared.finalCpuSeconds.store(threadCpuSecondsOf(GetCurrentThread()));
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
    m_shared.state.store(DecoderState::Failed);
}

bool VideoDecoder::Worker::open() {
    const DecoderSettings& settings = m_shared.settings;
    const std::filesystem::path& path = m_shared.path;
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(status)) {
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
        fail(result == AVERROR(ENOENT) ? DecodeFailure::FileNotFound : DecodeFailure::InvalidFile,
             fmt::format("cannot open the file: {}", detail::errorText(result)));
        return false;
    }
    m_format.reset(input);
    result = avformat_find_stream_info(m_format.get(), nullptr);
    if (result < 0) {
        if (m_stop.stop_requested()) {
            return false;
        }
        fail(DecodeFailure::InvalidFile, fmt::format("cannot read the streams: {}", detail::errorText(result)));
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
        m_declaredDuration = detail::toSeconds(stream->duration, m_timeBase);
    } else if (m_format->duration != AV_NOPTS_VALUE && m_format->duration > 0) {
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
    info.bitRate = stream->codecpar->bit_rate > 0 ? stream->codecpar->bit_rate : m_format->bit_rate;
    info.decoderThreads = m_codec->thread_count;
    spdlog::info("Video '{}': {} {}x{} {} (sample aspect {:.3f}), {:.3f} fps, {:.2f} s, {} kb/s, container {}; frames "
                 "{}x{} RGBA, {} decoder thread(s), queue of {}",
                 settings.name, info.codec, info.codedSize.width, info.codedSize.height, info.pixelFormat,
                 info.sampleAspect, info.frameRate, info.durationSeconds, info.bitRate / 1000, info.container,
                 info.outputSize.width, info.outputSize.height, info.decoderThreads, settings.queueCapacity);
    {
        const std::lock_guard lock(m_shared.infoMutex);
        m_shared.info = std::move(info);
    }
    m_shared.setState(DecoderState::Playing);
    resetPass(true);
    return true;
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

std::optional<double> VideoDecoder::Worker::takeSeekRequest() {
    const std::lock_guard lock(m_shared.controlMutex);
    return std::exchange(m_shared.seekRequest, std::nullopt);
}

bool VideoDecoder::Worker::waitWhilePaused() {
    std::unique_lock lock(m_shared.controlMutex);
    if (!m_shared.paused) {
        return true;
    }
    m_shared.setState(DecoderState::Paused);
    m_shared.control.wait(lock, m_stop, [this] { return !m_shared.paused || m_shared.seekRequest.has_value(); });
    if (m_stop.stop_requested()) {
        return false;
    }
    if (!m_shared.paused) {
        m_shared.setState(m_ended ? DecoderState::Ended : DecoderState::Playing);
    }
    return true;
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
        if (const std::optional<double> target = takeSeekRequest()) {
            if (!applySeek(*target)) {
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
    if (result == AVERROR_EOF) {
        return endOfStream();
    }
    if (result == AVERROR_EXIT || m_stop.stop_requested()) {
        av_packet_unref(m_packet.get());
        return false;
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
    const bool pushed = m_shared.queue.push(std::move(out), m_stop);
    waited += Clock::now() - pushStart;
    m_shared.queueWaitNanoseconds.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count(),
                                            std::memory_order_relaxed);
    if (pushed) {
        m_shared.framesQueued.fetch_add(1, std::memory_order_relaxed);
    }
    return pushed;
}

bool VideoDecoder::Worker::endOfStream() {
    // Drain: the decoder returns the frames it still holds (B-frame reordering, frame threads).
    if (!sendPacket(nullptr)) {
        return false;
    }
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
    if (m_firstPass && m_passFromStart && m_declaredDuration > 0.0) {
        const double span = m_passEnd - m_passStart.value_or(0.0);
        if (span < m_declaredDuration * kTruncationRatio - kTruncationSlackSeconds) {
            fail(DecodeFailure::Truncated,
                 fmt::format("the stream ends after {:.2f} s of the {:.2f} s it declares", span, m_declaredDuration));
            return false;
        }
    }
    if (!m_shared.settings.loop) {
        m_ended = true;
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
            fail(DecodeFailure::DecoderError,
                 fmt::format("cannot seek to {:.3f} s: {}", seconds, detail::errorText(result)));
        }
        return false;
    }
    avcodec_flush_buffers(m_codec.get());
    m_decodeTime = Clock::duration::zero();
    return true;
}

bool VideoDecoder::Worker::applySeek(double seconds) {
    const double target = std::max(0.0, seconds);
    m_shared.queue.clear();
    ++m_generation;
    m_shared.seeks.fetch_add(1, std::memory_order_relaxed);
    if (!seekTo(target)) {
        return false;
    }
    // A new generation: timestamps start again at the target, without the loop offset.
    m_loopOffset = 0.0;
    m_lastPts = -std::numeric_limits<double>::infinity();
    m_seekTarget = target;
    m_ended = false;
    resetPass(target <= 0.0);
    m_firstPass = false;
    m_shared.setState(DecoderState::Playing);
    spdlog::debug("Video '{}': seek to {:.3f} s (generation {})", m_shared.settings.name, target, m_generation);
    return true;
}

// --- VideoDecoder
// -------------------------------------------------------------------------------------------------------

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
    if (settings.name.empty()) {
        settings.name = core::pathToUtf8(path.filename());
    }
    detail::installLogBridge();

    auto shared = std::make_shared<Shared>(std::move(path), std::move(settings));
    shared->paused = shared->settings.startPaused;
    std::unique_ptr<VideoDecoder> decoder(new VideoDecoder(shared));
    try {
        decoder->m_thread = std::jthread([shared](std::stop_token stop) { Worker(*shared, std::move(stop)).run(); });
        decoder->m_threadHandle = decoder->m_thread.native_handle();
    } catch (const std::system_error& e) {
        return makeError(ErrorCode::Unknown, fmt::format("cannot start the decoder thread: {}", e.what()), context);
    }
    return decoder;
}

VideoDecoder::~VideoDecoder() {
    stop();
}

void VideoDecoder::stop() {
    if (m_thread.joinable()) {
        m_thread.request_stop(); // wakes every wait and interrupts FFmpeg's I/O
        m_shared->queue.close();
        m_shared->pool->close();
        m_thread.join();
        m_threadHandle = {};
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
    return m_shared->state.load(std::memory_order_acquire);
}

DecodeFailure VideoDecoder::failure() const noexcept {
    return m_shared->failure.load(std::memory_order_acquire);
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
    const std::lock_guard lock(shared.timingMutex);
    stats.decodeMilliseconds = shared.decodeTimes.summary();
    stats.convertMilliseconds = shared.convertTimes.summary();
    return stats;
}

double VideoDecoder::threadCpuSeconds() const {
    // The thread publishes its final CPU time before it ends, and stop() joins it (closing the handle) only after that:
    // while nothing is published, the handle is valid.
    const double final = m_shared->finalCpuSeconds.load();
    if (final >= 0.0) {
        return final;
    }
    return m_threadHandle != nullptr ? threadCpuSecondsOf(static_cast<HANDLE>(m_threadHandle)) : 0.0;
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
    m_shared->paused = true;
}

void VideoDecoder::resume() {
    {
        const std::lock_guard lock(m_shared->controlMutex);
        m_shared->paused = false;
    }
    m_shared->control.notify_all();
}

void VideoDecoder::seek(double seconds) {
    {
        const std::lock_guard lock(m_shared->controlMutex);
        m_shared->seekRequest = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
        // An ended stream plays again from now on (the thread takes the request at once).
        DecoderState ended = DecoderState::Ended;
        m_shared->state.compare_exchange_strong(ended, DecoderState::Playing);
    }
    m_shared->control.notify_all();
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
