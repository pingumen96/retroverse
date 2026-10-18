#include <rv/app/frame_timing.hpp>

#include <cmath>

namespace rv::app {

FrameTiming::FrameTiming(std::uint64_t warmUpFrames) : m_warmUpFrames(warmUpFrames) {}

void FrameTiming::openPhase(std::string_view name) {
    if (name.empty()) {
        return;
    }
    Phase* phase = find(name);
    if (phase == nullptr) {
        m_phases.push_back(Phase{std::string(name)});
        phase = &m_phases.back();
    }
    ++phase->depth;
    ++phase->opened;
    phase->touched = true;
}

bool FrameTiming::closePhase(std::string_view name) {
    Phase* phase = find(name);
    if (phase == nullptr || phase->depth == 0) {
        return false;
    }
    --phase->depth;
    return true;
}

bool FrameTiming::phaseOpen(std::string_view name) const {
    const Phase* phase = find(name);
    return phase != nullptr && phase->depth > 0;
}

void FrameTiming::recordFrame(std::uint64_t frameIndex, std::optional<double> milliseconds) {
    const bool timed = frameIndex >= m_warmUpFrames && milliseconds && std::isfinite(*milliseconds) &&
                       *milliseconds >= 0.0;
    if (timed) {
        m_milliseconds.push_back(*milliseconds);
    }
    for (Phase& phase : m_phases) {
        if (phase.depth > 0 || phase.touched) {
            ++phase.frames;
            if (timed) {
                phase.milliseconds.push_back(*milliseconds);
            }
        }
        phase.touched = false;
    }
}

FrameTimesRecord FrameTiming::runTimes() const {
    std::vector<double> samples = m_milliseconds;
    return FrameTimesRecord{m_warmUpFrames, summarizeTimes(samples)};
}

std::vector<PhaseRecord> FrameTiming::phases() const {
    std::vector<PhaseRecord> records;
    records.reserve(m_phases.size());
    for (const Phase& phase : m_phases) {
        std::vector<double> samples = phase.milliseconds;
        records.push_back(PhaseRecord{phase.name, phase.opened, phase.depth > 0, phase.frames, summarizeTimes(samples)});
    }
    return records;
}

FrameTiming::Phase* FrameTiming::find(std::string_view name) {
    for (Phase& phase : m_phases) {
        if (phase.name == name) {
            return &phase;
        }
    }
    return nullptr;
}

const FrameTiming::Phase* FrameTiming::find(std::string_view name) const {
    for (const Phase& phase : m_phases) {
        if (phase.name == name) {
            return &phase;
        }
    }
    return nullptr;
}

} // namespace rv::app
