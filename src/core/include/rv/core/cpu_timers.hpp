#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rv::core {

// CPU time of named sections of the main thread (I3 step 0d; ADR-0011 §9 and the budgets of docs/roadmap.md, I3): the
// time the main thread spends in a section during each frame, summarized over the run as p50, p99 and max for the run
// report's cpuSections.

/// The sections the budgets name. A section is any name; these are the ones the code and the budgets share.
namespace cpu_section {
/// The collection's planning: static groups, cluster culling, every view (I3 step 8; budget 0.3 ms per frame).
inline constexpr std::string_view kCollectionPlanning = "collectionPlanning";
/// Uploads of collection layers into the texture arrays (I3 steps 6 and 12; budget 1 ms per frame).
inline constexpr std::string_view kLayerUploads = "layerUploads";
/// The hero CRT's video frame upload (I3 step 11; budget p99 0.5 ms).
inline constexpr std::string_view kVideoUpload = "videoUpload";
/// The product UI: its update and its drawing into the window and the screenshots (budget 1 ms on quiet frames, I3
/// step 14a).
inline constexpr std::string_view kProductUi = "productUi";
/// The scene renderer's CPU side: every view the frame rendered (window and screenshots), from the culling to the
/// last GL call it queued; the collection's planning (once it exists) is part of it.
inline constexpr std::string_view kSceneSubmit = "sceneSubmit";
} // namespace cpu_section

/// Durations in milliseconds summarized by nearest rank: the value at rank ceil(fraction * count) of the sorted
/// samples (the first for a fraction of 0), clamped to [1, count].
struct DurationPercentiles {
    std::uint64_t count = 0;
    double p50Ms = 0.0;
    double p95Ms = 0.0;
    double p99Ms = 0.0;
    double maxMs = 0.0;

    bool operator==(const DurationPercentiles&) const = default;
};

/// The percentiles of `milliseconds`, which it sorts; nothing for none.
[[nodiscard]] std::optional<DurationPercentiles> summarizeDurations(std::vector<double>& milliseconds);

/// One section over the run: the frames during which it ran, and the percentiles of its time per frame (the sum of
/// every time it ran during the frame).
struct CpuSectionSummary {
    std::string name;
    /// DurationPercentiles::count is the frames during which the section ran.
    DurationPercentiles times;

    bool operator==(const CpuSectionSummary&) const = default;
};

/// Accumulates the CPU time of named sections per frame. A section runs any number of times per frame (add(), or a
/// Scope); endFrame() closes the frame: each section that ran during it records its total as one sample, and the
/// others record nothing (their percentiles describe the frames they ran in). Time spent in a section between two
/// endFrame() calls belongs to the frame the second one closes, so a section run before the first frame (an
/// application's initialization) counts towards the first; what runs after the last endFrame() is not recorded.
/// Samples are kept for the whole run (8 bytes per section and frame: an hour at 144 Hz is about 4 MiB per section).
/// Not thread-safe: one instance per thread (mainThreadCpuTimers() for the main thread).
class CpuTimers {
public:
    using Clock = std::chrono::steady_clock;
    /// Index of a section, stable for the life of the instance.
    using SectionId = std::size_t;

    /// The section called `name` (registered on first use; names are compared exactly).
    [[nodiscard]] SectionId section(std::string_view name);

    /// Adds `milliseconds` (negative or not finite: ignored) to the section's time in the current frame.
    void add(SectionId section, double milliseconds) noexcept;
    void add(std::string_view name, double milliseconds) { add(section(name), milliseconds); }

    /// Closes the current frame (see the class comment).
    void endFrame();

    /// The section's time in the frame being accumulated so far; nothing when it has not run since the last
    /// endFrame(), or for an unknown id.
    [[nodiscard]] std::optional<double> currentFrameMilliseconds(SectionId section) const;
    /// Frames closed so far.
    [[nodiscard]] std::uint64_t frames() const noexcept { return m_frames; }

    /// Every section that ran during at least one closed frame, ordered by name.
    [[nodiscard]] std::vector<CpuSectionSummary> summaries() const;

    /// Forgets every section and sample (tests; a new run in the same process).
    void clear() noexcept;

    /// Times the section from its construction to its destruction (or stop()) with Clock and adds the time to the
    /// current frame. Move-only; a moved-from scope adds nothing.
    class Scope {
    public:
        Scope(CpuTimers& timers, SectionId section) noexcept;
        Scope(Scope&& other) noexcept;
        Scope& operator=(Scope&&) = delete;
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope();

        /// Adds the time so far and stops; later calls do nothing.
        void stop() noexcept;

    private:
        CpuTimers* m_timers;
        SectionId m_section;
        Clock::time_point m_start;
    };

    /// A Scope of the section called `name`.
    [[nodiscard]] Scope scope(std::string_view name) { return Scope(*this, section(name)); }

private:
    struct Section {
        std::string name;
        /// Time in the frame being accumulated; nothing while the section has not run in it.
        std::optional<double> current;
        /// One sample per closed frame during which the section ran.
        std::vector<double> samples;
    };

    std::vector<Section> m_sections;
    std::uint64_t m_frames = 0;
};

/// The main thread's sections: the host closes their frame after every frame it presents (app::AppHost) and the run
/// report summarizes them. Use from the main thread only (core::assertMainThread logs other callers of
/// CpuSectionScope).
[[nodiscard]] CpuTimers& mainThreadCpuTimers() noexcept;

/// Times the section called `name` of mainThreadCpuTimers() for the scope's lifetime: `core::CpuSectionScope
/// timing(core::cpu_section::kProductUi);`. Off the main thread it logs an error and times nothing.
class CpuSectionScope {
public:
    explicit CpuSectionScope(std::string_view name);
    CpuSectionScope(const CpuSectionScope&) = delete;
    CpuSectionScope& operator=(const CpuSectionScope&) = delete;
    ~CpuSectionScope() = default;

private:
    std::optional<CpuTimers::Scope> m_scope;
};

} // namespace rv::core
