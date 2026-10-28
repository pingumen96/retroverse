#pragma once

#include <cstdint>
#include <span>

namespace rv::media {

/// Where a VideoDecoder puts its clip's sound (I5 step 5, ADR-0016 §9): stereo float frames, interleaved, at the rate
/// the decoder was given (DecoderSettings::audioSampleRate), resampled by swresample on the decoder's own thread (mono
/// up-mixed to both channels at its level, more than two channels down-mixed). rv_media knows no mixer: the application
/// adapts this to its own (rv_app connects rv_media and rv_audio).
class AudioSink {
public:
    virtual ~AudioSink() = default;

    /// Called on the decoder's thread: `interleaved` (stereo frames) whose first frame shows at `ptsSeconds` of the
    /// clip's timeline, the one its video frames carry (FrameTiming::ptsSeconds: loops keep growing, a seek starts again
    /// at its target), of seek generation `generation` (FrameTiming::generation). Must not block: a sink that cannot keep
    /// them drops them.
    virtual void write(std::span<const float> interleaved, double ptsSeconds, std::uint32_t generation) = 0;
};

} // namespace rv::media
