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
#include <rv/media/audio_sink.hpp>
#include <rv/media/frame_queue.hpp>
#include <rv/media/sample_stats.hpp>
#include <rv/media/video_frame.hpp>

namespace rv::media {

/// Life cycle of a VideoDecoder.
enum class DecoderState : std::uint8_t {
    Opening, ///< the thread is opening the file
    Playing, ///< decoding (or waiting for room in the queue)
    Paused,  ///< pause() was called (reported at once): the thread reads nothing more until resume()
    Ended,   ///< end of the stream without looping: the queued frames are the last ones
    Failed,  ///< see DecodeFailure: show the fallback picture (spec §9.5, SC-08)
    Stopped, ///< stop() was called: the thread ends on its own (waitForStoppedDecoders())
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

/// A first pass from the start that ends before this share of the video stream's own duration, less
/// kTruncatedPassSlackSeconds, fails as Truncated (ADR-0004 point 10); the check of a clip's sample applies the same
/// rule to its last frame (video_check.hpp).
inline constexpr double kTruncatedPassRatio = 0.9;
inline constexpr double kTruncatedPassSlackSeconds = 0.5;

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
    /// The sound (I5 step 5, ADR-0016 §9): the clip's first audio stream is decoded on the decoder's thread, resampled
    /// to audioSampleRate stereo float and written to the sink. A sound longer than the picture is trimmed at the
    /// picture's end (each loop's sound starts with its picture), and the last frames are drained at each pass's end.
    /// None: the audio packets are discarded undecoded.
    std::shared_ptr<AudioSink> audioSink;
    /// The sink's rate (8000..192000).
    int audioSampleRate = 48000;
    /// Whether the sound is decoded from the start (VideoDecoder::setAudioEnabled() changes it): a decoder whose sound
    /// nobody hears discards its audio packets, so that it costs no audio decoding.
    bool audioEnabled = true;
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
    /// The video stream's own duration (its own, or the DURATION tag Matroska writes per track), 0 when it declares
    /// none: what the truncation rule measures a pass against (I5 step 7: the check of a clip's sample).
    double streamDurationSeconds = 0.0;
    std::int64_t bitRate = 0;     ///< bits per second, 0 when unknown
    int decoderThreads = 1;
    /// The first audio stream (the one decoded with a sink): its codec, rate and channels; "" and 0 without one.
    std::string audioCodec;
    int audioSampleRate = 0;
    int audioChannels = 0;
    /// A sink was given and the stream's decoder opened: its sound reaches the sink.
    bool audioDecoded = false;
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
    std::uint64_t seeks = 0;          ///< seeks the thread carried out (several requested at once count once)
    /// Pixel storage allocations of the pool's frames: at most the pool's capacity unless the video changes size.
    std::uint64_t frameAllocations = 0;
    /// Time the thread waited for room in the queue (the consumer is behind or paused), seconds.
    double queueWaitSeconds = 0.0;
    /// From start() to the stream being open and described (streamInfo()), milliseconds; negative until then.
    double openMilliseconds = -1.0;
    /// From start() to the first frame handed to the queue, milliseconds; negative until then (NFR-06: the first
    /// preview frame within 2 s).
    double firstFrameMilliseconds = -1.0;
    /// Per frame: time in avcodec_send_packet / avcodec_receive_frame since the previous frame, milliseconds.
    SampleSummary decodeMilliseconds;
    /// Per frame: conversion to RGBA8 (sws_scale), milliseconds.
    SampleSummary convertMilliseconds;
    /// The sound: frames written to the sink (at its rate), audio packets discarded undecoded (no sink, or the sound
    /// disabled), audio packets or frames that failed (never a failure of the clip: its sound is skipped).
    std::uint64_t audioFramesWritten = 0;
    std::uint64_t audioPacketsDiscarded = 0;
    std::uint64_t audioDecodeErrors = 0;
};

/// Decodes the video stream of one file on its own thread (specification §9.4, §15.3): FFmpeg demuxing
/// (avformat), decoding with timestamps (avcodec_send_packet / avcodec_receive_frame, EAGAIN handled, end of stream
/// drained, flushed on seek and on every loop), conversion to RGBA8 scaled to DecoderSettings::maxHeight (swscale),
/// and frames pushed with their presentation timestamps into a bounded FrameQueue fed from a FramePool. Opening the
/// file happens on the thread too, so the caller never waits for the disk. No OpenGL: the render thread uploads the
/// frames (gl::VideoTexture). Errors never leave the thread: they end it with a DecodeFailure that the consumer reads
/// (state(), failure(), error()) to show its fallback picture.
///
/// The owner is the consumer: it pops the queue and calls pause(), resume(), seek() and stop() from one thread (the
/// render thread through VideoPlayback). None of them waits for the decoder's thread: stop() and the destructor ask
/// it to end and return, and the thread finishes on its own (at most the read or the frame it is busy with), so a
/// slow disk or network share never blocks the render thread (spec §9.4). waitForStoppedDecoders() waits for those
/// threads at application shutdown.
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
    /// stop(): never waits for the thread.
    ~VideoDecoder();

    /// The decoded frames, oldest first. The consumer pops them (VideoPlayback does, through a PresentationClock).
    [[nodiscard]] FrameQueue& frames() noexcept;
    [[nodiscard]] const FrameQueue& frames() const noexcept;

    /// Current state; cheap (atomic reads), for every frame.
    [[nodiscard]] DecoderState state() const noexcept;
    /// Why it failed (None unless state() is Failed, or the decoder failed before or while it was stopped); cheap.
    [[nodiscard]] DecodeFailure failure() const noexcept;
    /// Seek generation of the frames delivered from now on (FrameTiming::generation): 0, then the value the latest
    /// seek() returned. Cheap.
    [[nodiscard]] std::uint32_t generation() const noexcept;
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
    /// True until the decoder's thread has ended (stop() does not wait for it).
    [[nodiscard]] bool threadRunning() const noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] const std::string& name() const noexcept;
    [[nodiscard]] const DecoderSettings& settings() const noexcept;

    /// Stops reading packets until resume(); state() reports Paused at once. The frame being converted still reaches
    /// the queue, and so does the frame a seek made while paused aims at (the paused picture follows the seek).
    void pause();
    void resume();
    /// Decodes the sound into the sink from the next audio packet on, or discards the audio packets undecoded (the
    /// decoder's audio state starts again at the next enable: a discontinuity). Cheap; any thread.
    void setAudioEnabled(bool enabled) noexcept;
    [[nodiscard]] bool audioEnabled() const noexcept;
    /// Continues from `seconds` into the clip (clamped to >= 0). Returns the generation the frames after the seek carry
    /// (FrameTiming::generation, generation()). Takes effect at once, without waiting for the thread: the queued frames
    /// of the old position go back to the pool now (on the caller's thread, the consumer's), and a thread waiting for
    /// room in the queue discards the frame it holds. The thread then decodes from a key frame up to a second before
    /// the target (MP4 places key frames by decoding time: B-frames shown just before a key frame would be lost
    /// otherwise), drops the frames before it, and delivers frames from the one showing `seconds` on; while paused it
    /// delivers the first of them and waits. Also restarts an ended stream. A target past the last frame ends the pass:
    /// a looping decoder goes on from the start of the clip, another one ends.
    std::uint32_t seek(double seconds);
    /// Asks the thread to end and returns at once: every wait of the thread wakes, FFmpeg's I/O is interrupted between
    /// reads, and the thread finishes the read or the frame it is busy with on its own (waitForStoppedDecoders() waits
    /// for it). The state becomes Stopped unless the decoder failed. Idempotent; frames already queued can still be
    /// popped.
    void stop();

private:
    struct Shared;
    class Worker;
    struct HandleCloser {
        void operator()(void* handle) const noexcept;
    };
    friend Expected<StreamInfo> probeVideo(const std::filesystem::path& path, int maxHeight);

    explicit VideoDecoder(std::shared_ptr<Shared> shared) noexcept;

    /// State shared with the thread (which holds a reference of its own).
    std::shared_ptr<Shared> m_shared;
    std::jthread m_thread;
    /// A handle of the thread owned by the decoder (a duplicate: the thread object itself is handed over by stop()),
    /// valid after the thread ended: its CPU time and whether it still runs.
    std::unique_ptr<void, HandleCloser> m_threadHandle;
};

/// Waits until the thread of every stopped or destroyed VideoDecoder has ended (VideoDecoder::stop() never waits).
/// Call it at application shutdown, before the logger shuts down, never from the render loop: a thread may still be
/// finishing a slow read. Threads that have ended are also reclaimed as decoders stop, and at process exit.
void waitForStoppedDecoders();
/// How many times waitForStoppedDecoders() was called in the process so far (I4 step 6b: the hero CRT's report shows
/// that no call came before the run's end, so none from the render loop).
[[nodiscard]] std::uint64_t stoppedDecoderWaits() noexcept;

/// Opens `path` like a VideoDecoder and describes its video stream without decoding a frame, on the calling thread
/// (tests, tools, diagnostics). outputSize follows `maxHeight`. Errors: the DecodeFailure of an open that fails, as
/// errorCodeFor() maps it, the file as context.
[[nodiscard]] Expected<StreamInfo> probeVideo(const std::filesystem::path& path,
                                              int maxHeight = DecoderSettings::kDefaultMaxHeight);

/// The FFmpeg build linked in: "FFmpeg <version> (libavformat a.b.c, libavcodec ..., libswscale ..., libavutil ...)",
/// for logs and reports.
[[nodiscard]] std::string ffmpegVersionInfo();

} // namespace rv::media
