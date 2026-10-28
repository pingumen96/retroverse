#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rv::audio {

/// The mixer's rate and channels (ADR-0016 §9): every clip's audio is resampled to this, stereo float.
inline constexpr int kMixRate = 48000;
inline constexpr int kMixChannels = 2;
/// A slot's ring holds this much audio (ADR-0016 §9).
inline constexpr double kRingSeconds = 0.5;

/// Where the reader stands in an AudioRing: the media time of the next frame to read, its seek generation, and how
/// many frames follow it without a break in time (the rest of its chunk).
struct RingFront {
    double ptsSeconds = 0.0;
    std::uint32_t generation = 0;
    std::size_t frames = 0;
};

/// What an AudioRing went through (each count over its whole life).
struct RingStats {
    std::uint64_t framesWritten = 0;
    std::uint64_t framesRead = 0;
    /// Frames a write could not keep: the ring was full (the reader behind, or not reading), or its chunk list was.
    std::uint64_t overflowFrames = 0;
    /// Frames the reader threw away without playing them (skip(), an older generation, clear()).
    std::uint64_t skippedFrames = 0;
};

/// The decoded audio of one clip between the decoder's thread (the one writer) and the mixer (the one reader): a fixed
/// ring of interleaved stereo float frames, bounded (kRingSeconds by default: its memory never grows) and timestamped
/// in chunks (each write is a chunk: the media time of its first frame and its seek generation, as the video's frames
/// carry them). Lock-free: the writer only appends and never waits (frames that do not fit are dropped and counted, so
/// that a reader that stopped never blocks the decoder), the reader never waits for the writer. Neither allocates once
/// made.
class AudioRing {
public:
    /// A ring of `capacitySeconds` at `sampleRate` (at least one frame) with room for `maxChunks` writes in flight.
    explicit AudioRing(int sampleRate = kMixRate, double capacitySeconds = kRingSeconds, std::size_t maxChunks = 512);
    AudioRing(const AudioRing&) = delete;
    AudioRing& operator=(const AudioRing&) = delete;

    [[nodiscard]] int sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] std::size_t capacityFrames() const noexcept { return m_capacity; }

    // --- the writer (the decoder's thread)

    /// Appends `interleaved` (stereo frames) whose first frame shows at `ptsSeconds` of generation `generation`.
    /// Returns the frames kept: those beyond the free room are dropped (RingStats::overflowFrames), and the whole write
    /// when the chunk list is full.
    std::size_t write(std::span<const float> interleaved, double ptsSeconds, std::uint32_t generation);

    // --- the reader (the mixer)

    /// The next frame to read, past chunks of generations older than `minGeneration` (thrown away); none when nothing
    /// of that generation or later is there.
    [[nodiscard]] std::optional<RingFront> front(std::uint32_t minGeneration = 0);
    /// Copies the next frame into `frame` (two floats) without taking it; false when there is none.
    [[nodiscard]] bool peekFrame(float* frame);
    /// Takes up to `frames` frames of the current chunk into `out` (interleaved; may be null to skip them uncounted);
    /// returns the frames taken.
    std::size_t read(float* out, std::size_t frames);
    /// Throws away up to `frames` frames (across chunks); returns those thrown away.
    std::size_t skip(std::size_t frames);
    /// Throws away everything written so far.
    void clear();
    /// Frames written and not read yet.
    [[nodiscard]] std::size_t availableFrames() const;
    /// The media time just past the last frame written (its chunk's time plus its frames), of its generation; none
    /// when the ring is empty.
    [[nodiscard]] std::optional<RingFront> back() const;

    /// Counters (any thread).
    [[nodiscard]] RingStats stats() const;

private:
    struct Chunk {
        std::uint64_t start = 0; ///< absolute index of its first frame
        std::uint32_t frames = 0;
        std::uint32_t generation = 0;
        double ptsSeconds = 0.0;
    };

    /// The chunk the reader is in, advancing past finished ones; none when the reader caught up.
    [[nodiscard]] const Chunk* currentChunk();

    int m_sampleRate;
    std::size_t m_capacity;
    std::vector<float> m_samples;
    std::vector<Chunk> m_chunks;

    /// The writer's next frame (absolute), its own.
    std::uint64_t m_tail = 0;
    /// Chunks published by the writer (release) and finished by the reader.
    std::atomic<std::uint64_t> m_chunksWritten{0};
    std::atomic<std::uint64_t> m_chunksRead{0};
    /// The reader's next frame (absolute); the writer's free room is m_capacity - (m_tail - m_head).
    std::atomic<std::uint64_t> m_head{0};

    std::atomic<std::uint64_t> m_framesWritten{0};
    std::atomic<std::uint64_t> m_framesRead{0};
    std::atomic<std::uint64_t> m_overflowFrames{0};
    std::atomic<std::uint64_t> m_skippedFrames{0};
};

} // namespace rv::audio
