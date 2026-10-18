#include <rv/core/cpu_timers.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <rv/core/main_thread.hpp>

namespace rv::core {

std::optional<DurationPercentiles> summarizeDurations(std::vector<double>& milliseconds) {
    if (milliseconds.empty()) {
        return std::nullopt;
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    const std::size_t count = milliseconds.size();
    const auto rank = [&](double fraction) {
        const auto index = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(count)));
        return milliseconds[std::min(count - 1, index > 0 ? index - 1 : 0)];
    };
    return DurationPercentiles{static_cast<std::uint64_t>(count), rank(0.50), rank(0.95), rank(0.99),
                               milliseconds.back()};
}

CpuTimers::SectionId CpuTimers::section(std::string_view name) {
    for (SectionId index = 0; index < m_sections.size(); ++index) {
        if (m_sections[index].name == name) {
            return index;
        }
    }
    m_sections.push_back(Section{std::string(name), std::nullopt, {}});
    return m_sections.size() - 1;
}

void CpuTimers::add(SectionId section, double milliseconds) noexcept {
    if (section >= m_sections.size() || !std::isfinite(milliseconds) || milliseconds < 0.0) {
        return;
    }
    std::optional<double>& current = m_sections[section].current;
    current = current.value_or(0.0) + milliseconds;
}

void CpuTimers::endFrame() {
    for (Section& section : m_sections) {
        if (section.current) {
            section.samples.push_back(*section.current);
            section.current.reset();
        }
    }
    ++m_frames;
}

std::optional<double> CpuTimers::currentFrameMilliseconds(SectionId section) const {
    if (section >= m_sections.size()) {
        return std::nullopt;
    }
    return m_sections[section].current;
}

std::vector<CpuSectionSummary> CpuTimers::summaries() const {
    std::vector<CpuSectionSummary> result;
    for (const Section& section : m_sections) {
        std::vector<double> samples = section.samples;
        if (const std::optional<DurationPercentiles> times = summarizeDurations(samples)) {
            result.push_back(CpuSectionSummary{section.name, *times});
        }
    }
    std::sort(result.begin(), result.end(),
              [](const CpuSectionSummary& a, const CpuSectionSummary& b) { return a.name < b.name; });
    return result;
}

void CpuTimers::clear() noexcept {
    m_sections.clear();
    m_frames = 0;
}

CpuTimers::Scope::Scope(CpuTimers& timers, SectionId section) noexcept
    : m_timers(&timers), m_section(section), m_start(Clock::now()) {}

CpuTimers::Scope::Scope(Scope&& other) noexcept
    : m_timers(std::exchange(other.m_timers, nullptr)), m_section(other.m_section), m_start(other.m_start) {}

CpuTimers::Scope::~Scope() {
    stop();
}

void CpuTimers::Scope::stop() noexcept {
    if (m_timers == nullptr) {
        return;
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - m_start).count();
    std::exchange(m_timers, nullptr)->add(m_section, milliseconds);
}

CpuTimers& mainThreadCpuTimers() noexcept {
    static CpuTimers timers;
    return timers;
}

CpuSectionScope::CpuSectionScope(std::string_view name) {
    if (!isMainThread()) {
        assertMainThread("CpuSectionScope");
        return;
    }
    CpuTimers& timers = mainThreadCpuTimers();
    m_scope.emplace(timers, timers.section(name));
}

} // namespace rv::core
