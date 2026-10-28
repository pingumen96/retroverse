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
    const bool counted = frameIndex >= m_warmUpFrames;
    const bool timed = counted && milliseconds && std::isfinite(*milliseconds) && *milliseconds >= 0.0;
    const std::optional<GpuSample> gpu = counted ? m_pendingGpu : std::nullopt;
    m_pendingGpu.reset();
    if (timed) {
        m_milliseconds.add(*milliseconds);
    }
    if (gpu) {
        addGpuSample(m_gpu, *gpu);
    }
    for (Phase& phase : m_phases) {
        if (phase.depth > 0 || phase.touched) {
            ++phase.frames;
            if (timed) {
                phase.milliseconds.add(*milliseconds);
            }
            if (gpu) {
                addGpuSample(phase.gpu, *gpu);
            }
        }
        phase.touched = false;
    }
}

void FrameTiming::recordGpuPasses(const gl::FrameStats& stats) {
    GpuSample sample{stats.shadowGpuMilliseconds, stats.depthGpuMilliseconds, stats.ssaoGpuMilliseconds,
                     stats.sceneGpuMilliseconds,  stats.postGpuMilliseconds,  stats.outlineGpuMilliseconds,
                     std::nullopt};
    double total = 0.0;
    bool any = false;
    for (std::size_t pass = 0; pass + 1 < kGpuPassCount; ++pass) {
        std::optional<double>& value = sample[pass];
        if (value && (!std::isfinite(*value) || *value < 0.0)) {
            value.reset();
        }
        if (value) {
            total += *value;
            any = true;
        }
    }
    if (any) {
        sample[kGpuPassCount - 1] = total;
    }
    m_pendingGpu = sample;
}

void FrameTiming::addGpuSample(GpuSamples& samples, const GpuSample& sample) {
    for (std::size_t pass = 0; pass < kGpuPassCount; ++pass) {
        if (sample[pass]) {
            samples[pass].add(*sample[pass]);
        }
    }
}

std::vector<GpuPassRecord> FrameTiming::gpuPassRecords(const GpuSamples& samples) {
    std::vector<GpuPassRecord> records;
    for (std::size_t pass = 0; pass < kGpuPassCount; ++pass) {
        if (const std::optional<TimeSummary> times = samples[pass].summary()) {
            records.push_back(GpuPassRecord{std::string(kGpuPassNames[pass]), *times});
        }
    }
    return records;
}

FrameTimesRecord FrameTiming::runTimes() const {
    return FrameTimesRecord{m_warmUpFrames, m_milliseconds.summary(), gpuPassRecords(m_gpu)};
}

std::vector<PhaseRecord> FrameTiming::phases() const {
    std::vector<PhaseRecord> records;
    records.reserve(m_phases.size());
    for (const Phase& phase : m_phases) {
        records.push_back(PhaseRecord{phase.name, phase.opened, phase.depth > 0, phase.frames,
                                      phase.milliseconds.summary(), gpuPassRecords(phase.gpu)});
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
