#include <rv/audio/audio_ring.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rv::audio {

AudioRing::AudioRing(int sampleRate, double capacitySeconds, std::size_t maxChunks)
    : m_sampleRate(std::max(1, sampleRate)),
      m_capacity(std::max<std::size_t>(
          1, static_cast<std::size_t>(std::llround(std::max(0.0, capacitySeconds) * static_cast<double>(m_sampleRate))))),
      m_samples(m_capacity * kMixChannels, 0.0f), m_chunks(std::max<std::size_t>(1, maxChunks)) {}

std::size_t AudioRing::write(std::span<const float> interleaved, double ptsSeconds, std::uint32_t generation) {
    const std::size_t frames = interleaved.size() / kMixChannels;
    if (frames == 0) {
        return 0;
    }
    const std::uint64_t chunksWritten = m_chunksWritten.load(std::memory_order_relaxed);
    if (chunksWritten - m_chunksRead.load(std::memory_order_acquire) >= m_chunks.size()) {
        m_overflowFrames.fetch_add(frames, std::memory_order_relaxed);
        return 0;
    }
    const std::uint64_t used = m_tail - m_head.load(std::memory_order_acquire);
    const std::size_t room = used >= m_capacity ? 0 : m_capacity - static_cast<std::size_t>(used);
    const std::size_t kept = std::min(frames, room);
    if (kept < frames) {
        m_overflowFrames.fetch_add(frames - kept, std::memory_order_relaxed);
    }
    if (kept == 0) {
        return 0;
    }
    // Copied in at most two pieces around the ring's end.
    const std::size_t at = static_cast<std::size_t>(m_tail % m_capacity);
    const std::size_t first = std::min(kept, m_capacity - at);
    std::memcpy(m_samples.data() + at * kMixChannels, interleaved.data(), first * kMixChannels * sizeof(float));
    if (kept > first) {
        std::memcpy(m_samples.data(), interleaved.data() + first * kMixChannels,
                    (kept - first) * kMixChannels * sizeof(float));
    }
    m_chunks[chunksWritten % m_chunks.size()] =
        Chunk{m_tail, static_cast<std::uint32_t>(kept), generation, ptsSeconds};
    m_tail += kept;
    m_framesWritten.fetch_add(kept, std::memory_order_relaxed);
    m_chunksWritten.store(chunksWritten + 1, std::memory_order_release);
    return kept;
}

const AudioRing::Chunk* AudioRing::currentChunk() {
    const std::uint64_t written = m_chunksWritten.load(std::memory_order_acquire);
    std::uint64_t read = m_chunksRead.load(std::memory_order_relaxed);
    const std::uint64_t head = m_head.load(std::memory_order_relaxed);
    while (read < written) {
        const Chunk& chunk = m_chunks[read % m_chunks.size()];
        if (head < chunk.start + chunk.frames) {
            return &chunk;
        }
        ++read;
        m_chunksRead.store(read, std::memory_order_release);
    }
    return nullptr;
}

std::optional<RingFront> AudioRing::front(std::uint32_t minGeneration) {
    for (;;) {
        const Chunk* chunk = currentChunk();
        if (chunk == nullptr) {
            return std::nullopt;
        }
        const std::uint64_t head = m_head.load(std::memory_order_relaxed);
        const std::uint64_t consumed = head - chunk->start;
        const std::uint64_t left = chunk->frames - consumed;
        if (chunk->generation < minGeneration) {
            // A chunk of a position the video left: never played.
            m_skippedFrames.fetch_add(left, std::memory_order_relaxed);
            m_head.store(head + left, std::memory_order_release);
            continue;
        }
        return RingFront{chunk->ptsSeconds + static_cast<double>(consumed) / static_cast<double>(m_sampleRate),
                         chunk->generation, static_cast<std::size_t>(left)};
    }
}

bool AudioRing::peekFrame(float* frame) {
    if (currentChunk() == nullptr) {
        return false;
    }
    const std::size_t at = static_cast<std::size_t>(m_head.load(std::memory_order_relaxed) % m_capacity);
    frame[0] = m_samples[at * kMixChannels];
    frame[1] = m_samples[at * kMixChannels + 1];
    return true;
}

std::size_t AudioRing::read(float* out, std::size_t frames) {
    const Chunk* chunk = currentChunk();
    if (chunk == nullptr || frames == 0) {
        return 0;
    }
    const std::uint64_t head = m_head.load(std::memory_order_relaxed);
    const std::size_t taken = std::min<std::size_t>(frames, static_cast<std::size_t>(chunk->start + chunk->frames - head));
    if (out != nullptr) {
        const std::size_t at = static_cast<std::size_t>(head % m_capacity);
        const std::size_t first = std::min(taken, m_capacity - at);
        std::memcpy(out, m_samples.data() + at * kMixChannels, first * kMixChannels * sizeof(float));
        if (taken > first) {
            std::memcpy(out + first * kMixChannels, m_samples.data(), (taken - first) * kMixChannels * sizeof(float));
        }
        m_framesRead.fetch_add(taken, std::memory_order_relaxed);
    }
    m_head.store(head + taken, std::memory_order_release);
    return taken;
}

std::size_t AudioRing::skip(std::size_t frames) {
    std::size_t skipped = 0;
    while (skipped < frames) {
        const std::size_t taken = read(nullptr, frames - skipped);
        if (taken == 0) {
            break;
        }
        skipped += taken;
    }
    m_skippedFrames.fetch_add(skipped, std::memory_order_relaxed);
    return skipped;
}

void AudioRing::clear() {
    const std::uint64_t written = m_chunksWritten.load(std::memory_order_acquire);
    if (written == 0) {
        return;
    }
    const Chunk& last = m_chunks[(written - 1) % m_chunks.size()];
    const std::uint64_t end = last.start + last.frames;
    const std::uint64_t head = m_head.load(std::memory_order_relaxed);
    if (end > head) {
        m_skippedFrames.fetch_add(end - head, std::memory_order_relaxed);
        m_head.store(end, std::memory_order_release);
    }
    m_chunksRead.store(written, std::memory_order_release);
}

std::size_t AudioRing::availableFrames() const {
    const std::uint64_t written = m_chunksWritten.load(std::memory_order_acquire);
    if (written == 0) {
        return 0;
    }
    const Chunk& last = m_chunks[(written - 1) % m_chunks.size()];
    const std::uint64_t end = last.start + last.frames;
    const std::uint64_t head = m_head.load(std::memory_order_acquire);
    return end > head ? static_cast<std::size_t>(end - head) : 0;
}

std::optional<RingFront> AudioRing::back() const {
    const std::uint64_t written = m_chunksWritten.load(std::memory_order_acquire);
    if (written == 0 || availableFrames() == 0) {
        return std::nullopt;
    }
    const Chunk& last = m_chunks[(written - 1) % m_chunks.size()];
    return RingFront{last.ptsSeconds + static_cast<double>(last.frames) / static_cast<double>(m_sampleRate),
                     last.generation, 0};
}

RingStats AudioRing::stats() const {
    return RingStats{m_framesWritten.load(std::memory_order_relaxed), m_framesRead.load(std::memory_order_relaxed),
                     m_overflowFrames.load(std::memory_order_relaxed),
                     m_skippedFrames.load(std::memory_order_relaxed)};
}

} // namespace rv::audio
