#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <rv/core/expected.hpp>
#include <rv/media/frame_queue.hpp>
#include <rv/media/sample_stats.hpp>
#include <rv/media/video_frame.hpp>

namespace rv::media {

/// Life cycle of a VideoDecoder.
enum class DecoderState : std::uint8_t {
    Opening, ///< the thread is opening the file
    Playing, ///< decoding (or waiting for room in the queue)
    Paused,  ///< pause(): the thread waits before reading on
    Ended,   ///< end of the stream without looping: the queued frames are the last ones
    Failed,  ///< see DecodeFailure: show the fallback picture (spec §9.5, SC-08)
    Stopped, ///< stop() ended the thread
};

[[nodiscard]] std::string_view toString(DecoderState state) noexcept;

/// Why a decoder failed. Frames delivered before the failure stay in the queue.
enum class DecodeFailure : std::uint8_t {
    None,
    FileNotFound,     ///< no file at the path (or not a regular file)
    InvalidFile,      ///< empty, or not a container FFmpeg recognizes, or its header is damaged
    NoVideoStream,    ///< a media file without a video stream (audio only, or only cover art)
    UnsupportedCodec, ///< no decoder for the video stream's codec in this FFmpeg build
    CorruptData,      ///< too many packets failed to demux or decode (DecoderSettings), or no frame decodes at all
    Truncated,        ///< the file ends in the middle of the stream (cut short, still being written)
    DecoderError,     ///< FFmpeg failed otherwise (out of memory, a conversion that cannot be set up)
};

[[nodiscard]] std::string_view toString(DecodeFailure failure) noexcept;

/// Error code of rv::Error that matches a failure (NotFound, ParseError, Unsupported, ...).
[[nodiscard]] ErrorCode errorCodeFor(DecodeFailure failure) noexcept;

/// How a VideoDecoder decodes.
struct DecoderSettings {
    static constexpr int kDefaultMaxHeight = 720;
    static constexpr int kMaxThreadCount = 16;
    static constexpr std::size_t kMaxQueueCapacity = 64;

    /// Frames taller than this are scaled down to it, keeping their aspect ratio (spec §9.5: 480p or 720p previews);
    /// never scaled up (scaledFrameSize). 0: no limit.
    int maxHeight = kDefaultMaxHeight;
    /// Frames decoded ahead (FrameQueue capacity, 1..kMaxQueueCapacity). The decoder's memory is bounded by it: its
    /// FramePool holds queueCapacity + 2 frames (one being converted, one held by the consumer).
    std::size_t queueCapacity = FrameQueue::kDefaultCapacity;
    /// FFmpeg decoding threads (AVCodecContext::thread_count, 1..kMaxThreadCount). 1 decodes on the decoder's own
    /// thread only, so its CPU time is the decoder's (spec §9.5 budgets decoders, not threads).
    int threadCount = 1;
    /// At the end of the stream, start again from the beginning; timestamps keep growing (loop offset).
    bool loop = true;
    /// Open the file, then wait for resume() before decoding.
    bool startPaused = false;
    /// Packets or frames in a row that may fail to demux or decode before the stream is declared corrupt.
    int maxConsecutiveErrors = 16;
    /// Once a clean frame has been decoded, corruptErrorThreshold errors among the last corruptErrorWindow decoding
    /// events (1..64; an event is a decoded frame or a packet or frame that failed) declare the stream corrupt. An
    /// error is a packet or frame FFmpeg fails to decode, or a frame it flags as damaged (concealed blocks,
    /// AVFrame::decode_error_flags): a picture full of broken blocks is worse than the fallback picture. Damage before
    /// the first clean frame (a clip cut before its first key frame) counts only towards maxConsecutiveErrors.
    int corruptErrorWindow = 32;
    int corruptErrorThreshold = 3;
    /// Per-frame decode and conversion times kept for the statistics (DecoderStats).
    std::size_t timingCapacity = 8192;
    /// Name in logs and of the thread; the file name when empty.
    std::string name;
};

/// What the decoder found in the file, known once it is open.
struct StreamInfo {
    std::string container;   ///< demuxer names ("mov,mp4,m4a,3gp,3g2,mj2")
    std::string codec;       ///< decoder name ("h264")
    std::string pixelFormat; ///< decoded pixel format ("yuv420p")
    FrameSize codedSize;     ///< pixels in the stream
    FrameSize outputSize;    ///< pixels of the delivered frames (scaledFrameSize)
    double sampleAspect = 1.0;
    double frameRate = 0.0;       ///< frames per second as FFmpeg guesses it, 0 when unknown
    double durationSeconds = 0.0; ///< of the video stream (or the container), 0 when unknown
    std::int64_t bitRate = 0;     ///< bits per second, 0 when unknown
    int decoderThreads = 1;
};

/// Counters of a decoder (VideoDecoder::stats()).
struct DecoderStats {
    std::uint64_t packetsRead = 0;
    std::uint64_t framesDecoded = 0;  ///< frames out of FFmpeg
    std::uint64_t framesQueued = 0;   ///< frames handed to the queue
    std::uint64_t framesSkipped = 0;  ///< decoded before a seek target and thrown away
    std::uint64_t decodeErrors = 0;   ///< packets and frames FFmpeg could not demux or decode
    std::uint64_t damagedFrames = 0;  ///< frames FFmpeg decoded with errors (concealed, missing references)
    std::uint64_t corruptPackets = 0; ///< packets the demuxer flagged corrupt
    std::uint64_t loops = 0;          ///< times the clip started again
    std::uint64_t seeks = 0;
    /// Pixel storage allocations of the pool's frames: at most the pool's capacity unless the video changes size.
    std::uint64_t frameAllocations = 0;
    /// Time the thread waited for room in the queue (the consumer is behind or paused), seconds.
    double queueWaitSeconds = 0.0;
    /// Per frame: time in avcodec_send_packet / avcodec_receive_frame since the previous frame, milliseconds.
    SampleSummary decodeMilliseconds;
    /// Per frame: conversion to RGBA8 (sws_scale), milliseconds.
    SampleSummary convertMilliseconds;
};

/// Decodes the video stream of one file on its own thread (specification §9.4, §15.3): FFmpeg demuxing
/// (avformat), decoding with timestamps (avcodec_send_packet / avcodec_receive_frame, EAGAIN handled, end of stream
/// drained, flushed on seek and on every loop), conversion to RGBA8 scaled to DecoderSettings::maxHeight (swscale),
/// and frames pushed with their presentation timestamps into a bounded FrameQueue fed from a FramePool. Opening the
/// file happens on the thread too, so the caller never waits for the disk. No OpenGL: the render thread uploads the
/// frames (gl::VideoTexture). Errors never leave the thread: they end it with a DecodeFailure that the consumer reads
/// (state(), failure(), error()) to show its fallback picture. stop() (and the destructor) ends the thread promptly:
/// waits wake on the stop request and FFmpeg's I/O is interrupted.
///
/// Files are opened read-only through FFmpeg's file protocol only (no network or other protocol can be reached from a
/// file's content); paths are UTF-8, so non-ASCII names work on Windows.
class VideoDecoder {
public:
    /// Starts the thread that decodes `path`. Errors: InvalidArgument for an empty path or settings out of range
    /// (maxHeight < 0, queueCapacity or threadCount outside their ranges, maxConsecutiveErrors < 1, corruptErrorWindow
    /// outside 1..64, corruptErrorThreshold < 1). A file that cannot be opened or decoded is not an error here: the
    /// decoder ends in DecoderState::Failed.
    [[nodiscard]] static Expected<std::unique_ptr<VideoDecoder>> start(std::filesystem::path path,
                                                                       DecoderSettings settings = {});

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) = delete;
    VideoDecoder& operator=(VideoDecoder&&) = delete;
    /// stop().
    ~VideoDecoder();

    /// The decoded frames, oldest first. The consumer pops them (VideoPlayback does, through a PresentationClock).
    [[nodiscard]] FrameQueue& frames() noexcept;
    [[nodiscard]] const FrameQueue& frames() const noexcept;

    /// Current state; cheap (an atomic read), for every frame.
    [[nodiscard]] DecoderState state() const noexcept;
    /// Why it failed (None unless state() is Failed, or was before stop()); cheap.
    [[nodiscard]] DecodeFailure failure() const noexcept;
    /// The failure with FFmpeg's message, the file as context; nothing without a failure. Copies strings: not for
    /// every frame.
    [[nodiscard]] std::optional<Error> error() const;
    /// The stream, once opened; copies strings: not for every frame.
    [[nodiscard]] std::optional<StreamInfo> streamInfo() const;
    /// Counters and timing summaries so far (sorts the timing samples: not for every frame).
    [[nodiscard]] DecoderStats stats() const;
    /// CPU time the decoder's thread used so far, user and kernel, seconds (Windows: GetThreadTimes); also after the
    /// thread ended. FFmpeg's own threads (DecoderSettings::threadCount > 1) are not included.
    [[nodiscard]] double threadCpuSeconds() const;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const DecoderSettings& settings() const noexcept;

    /// Stops reading packets until resume() (frames already decoding still reach the queue).
    void pause();
    void resume();
    /// Continues from `seconds` into the clip (clamped to >= 0): the queued frames are discarded, the decoder seeks to
    /// the key frame before it, drops the frames decoded before it, and the next frames carry the next generation
    /// (FrameTiming::generation) with timestamps from `seconds` on. Also restarts an ended stream. A target past the
    /// last frame ends the pass: a looping decoder goes on from the start of the clip, another one ends.
    void seek(double seconds);
    /// Ends the thread and waits for it: promptly, since every wait wakes at once and FFmpeg's I/O is interrupted (at
    /// most the packet being decoded is finished). Idempotent; frames already queued can still be popped.
    void stop();

private:
    struct Shared;
    class Worker;
    friend Expected<StreamInfo> probeVideo(const std::filesystem::path& path, int maxHeight);

    explicit VideoDecoder(std::shared_ptr<Shared> shared) noexcept;

    /// State shared with the thread (which holds a reference of its own).
    std::shared_ptr<Shared> m_shared;
    std::jthread m_thread;
    /// The thread's handle, for its CPU time while it runs (it publishes its final CPU time before it ends).
    std::jthread::native_handle_type m_threadHandle{};
};

/// Opens `path` like a VideoDecoder and describes its video stream without decoding a frame, on the calling thread
/// (tests, tools, diagnostics). outputSize follows `maxHeight`. Errors: the DecodeFailure of an open that fails, as
/// errorCodeFor() maps it, the file as context.
[[nodiscard]] Expected<StreamInfo> probeVideo(const std::filesystem::path& path,
                                              int maxHeight = DecoderSettings::kDefaultMaxHeight);

/// The FFmpeg build linked in: "FFmpeg <version> (libavformat a.b.c, libavcodec ..., libswscale ..., libavutil ...)",
/// for logs and reports.
[[nodiscard]] std::string ffmpegVersionInfo();

} // namespace rv::media
