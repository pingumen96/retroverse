#include <rv/media/video_playback.hpp>

#include <utility>

namespace rv::media {

/// The decoder's queue as the clock's FrameSource: the frame presented becomes the playback's pending frame, the ones
/// skipped go back to the pool at once.
class VideoPlayback::QueueSource final : public FrameSource {
public:
    explicit QueueSource(VideoPlayback& playback) noexcept : m_playback(playback) {}

    [[nodiscard]] std::optional<FrameTiming> peek(std::size_t index) const override {
        return m_playback.m_decoder->frames().peek(index);
    }

    void pop(bool present) override {
        PooledFrame frame = m_playback.m_decoder->frames().pop();
        if (present && frame) {
            if (m_playback.m_pending) {
                ++m_playback.m_superseded; // presented, but never uploaded
            }
            m_playback.m_pending = std::move(frame);
        }
    }

private:
    VideoPlayback& m_playback;
};

VideoPlayback::VideoPlayback(std::unique_ptr<VideoDecoder> decoder, ClockSettings clock)
    : m_decoder(std::move(decoder)), m_clock(clock) {}

VideoPlayback::VideoPlayback(VideoPlayback&&) noexcept = default;
VideoPlayback& VideoPlayback::operator=(VideoPlayback&&) noexcept = default;

VideoPlayback::~VideoPlayback() {
    // Frames go back to the pool before the decoder stops; the pool outlives both anyway.
    m_pending.reset();
}

PlaybackUpdate VideoPlayback::update(double nowSeconds) {
    PlaybackUpdate result;
    if (!m_decoder) {
        return result;
    }
    const DecoderState state = m_decoder->state();
    const bool sourceEnded =
        state == DecoderState::Ended || state == DecoderState::Failed || state == DecoderState::Stopped;
    QueueSource source(*this);
    result.clock = m_clock.update(nowSeconds, source, sourceEnded);
    result.frame = m_pending.get();
    return result;
}

void VideoPlayback::releaseFrame() noexcept {
    m_pending.reset();
}

void VideoPlayback::pause(double nowSeconds) {
    m_clock.pause(nowSeconds);
    if (m_decoder) {
        m_decoder->pause();
    }
}

void VideoPlayback::resume(double nowSeconds) {
    m_clock.resume(nowSeconds);
    if (m_decoder) {
        m_decoder->resume();
    }
}

bool VideoPlayback::failed() const noexcept {
    return m_decoder && m_decoder->failure() != DecodeFailure::None;
}

} // namespace rv::media
