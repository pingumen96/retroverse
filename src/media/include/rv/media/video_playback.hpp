#pragma once

#include <cstdint>
#include <memory>

#include <rv/media/presentation_clock.hpp>
#include <rv/media/video_decoder.hpp>
#include <rv/media/video_frame.hpp>

namespace rv::media {

/// Result of VideoPlayback::update().
struct PlaybackUpdate {
    /// The frame to show that has not been uploaded yet (the newest the clock presented); null when the picture on
    /// screen is still current. Valid until the next update(), seek() or releaseFrame().
    const VideoFrame* frame = nullptr;
    /// What the clock did at this update.
    ClockUpdate clock;
};

/// One video as the render thread plays it: a VideoDecoder and the PresentationClock that picks its frames by
/// timestamp. Each update() takes the due frames from the decoder's queue (never waiting for the decoder) and returns
/// the frame to upload when it changed; the caller uploads it (gl::VideoTexture) and calls releaseFrame() so that the
/// frame goes back to the decoder's pool. A frame not released yet (the upload was skipped) stays pending and is
/// replaced by a newer one at a later update (counted by supersededFrames()). The playback is the decoder's consumer:
/// call the decoder's controls from the render thread too. GPU-free; render thread only.
class VideoPlayback {
public:
    /// Plays the frames of `decoder` (non-null) with a clock tuned by `clock`.
    explicit VideoPlayback(std::unique_ptr<VideoDecoder> decoder, ClockSettings clock = {});
    VideoPlayback(const VideoPlayback&) = delete;
    VideoPlayback& operator=(const VideoPlayback&) = delete;
    VideoPlayback(VideoPlayback&&) noexcept;
    VideoPlayback& operator=(VideoPlayback&&) noexcept;
    /// Stops the decoder without waiting for its thread (VideoDecoder::stop()).
    ~VideoPlayback();

    /// Advances to render time `nowSeconds` (monotonic seconds). Allocates nothing.
    [[nodiscard]] PlaybackUpdate update(double nowSeconds);
    /// The pending frame was uploaded: give it back to the decoder's pool.
    void releaseFrame() noexcept;

    /// Pauses the clock and the decoder at render time `nowSeconds` (the picture stays: spec §9.3 "keep the frame
    /// visible").
    void pause(double nowSeconds);
    void resume(double nowSeconds);
    /// Continues from `seconds` into the clip (VideoDecoder::seek()). No frame of the old position is shown or
    /// uploaded after this call (a pending one is dropped); the frame at the target is shown as soon as it is decoded,
    /// and while paused it becomes the paused picture. A seek made through decoder() directly is followed the same way
    /// at the next update().
    void seek(double seconds);

    /// Show the fallback picture instead of the video (spec §9.5, SC-08): the decoder failed, and every frame it
    /// decoded before failing has been shown and has had its time (ADR-0004: those frames come first). Set by update();
    /// cheap.
    [[nodiscard]] bool failed() const noexcept { return m_failed; }

    [[nodiscard]] VideoDecoder& decoder() noexcept { return *m_decoder; }
    [[nodiscard]] const VideoDecoder& decoder() const noexcept { return *m_decoder; }
    [[nodiscard]] const PresentationClock& clock() const noexcept { return m_clock; }
    /// Frames the clock presented that were replaced before they could be uploaded.
    [[nodiscard]] std::uint64_t supersededFrames() const noexcept { return m_superseded; }

private:
    class QueueSource;

    /// Adopts the decoder's latest seek: the clock waits for its frames and a pending frame of an older one is dropped.
    void followSeeks() noexcept;
    /// True once the decoder failed and nothing it delivered is left to show at render time `nowSeconds`.
    [[nodiscard]] bool drainedAfterFailure(double nowSeconds) const;

    std::unique_ptr<VideoDecoder> m_decoder;
    PresentationClock m_clock;
    PooledFrame m_pending;
    std::uint64_t m_superseded = 0;
    bool m_failed = false;
};

} // namespace rv::media
