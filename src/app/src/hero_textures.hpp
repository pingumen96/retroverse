#pragma once

// The hero textures (I3 step 12; ADR-0011 §8): the focused, pulled-out or inspected item's cover at its source size up
// to 1,024 px (app::heroPictureSize: the front label's aspect, the cover fitted without stretching), RGBA8 sRGB with
// mips, a gl::Texture2D recorded under GpuMemoryKind::HeroTextures, in a least-recently-used set of 8 (app::HeroLru: at
// most about 32 MiB; the one shown is never evicted). Loaded on demand on the worker pool (read with
// core::readFileShared, never written, never cached on disk), at a priority above the covers' layers, and uploaded on
// the main thread, one a round. While the media root is unknown nothing is asked (the item keeps its layer); a cover
// that cannot be read or decoded is not asked again. Paused (no job, no upload) while a launched game runs. The card
// (I3 step 14a) and the media viewer (14b) take their cover from here. Main thread only, but for its jobs.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <rv/app/collection_layers.hpp>
#include <rv/app/cover_requests.hpp>
#include <rv/app/run_report.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/texture.hpp>

namespace rv::app {

class HeroTextures {
public:
    struct Options {
        /// The covers are read under it; none while the media are unknown (nothing is asked then).
        std::optional<std::filesystem::path> mediaRoot;
        /// Called by a job once its picture is posted (AppContext::wakeCallback(): thread-safe).
        std::function<void()> wake;
        std::size_t capacity = HeroLru::kDefaultCapacity;
        int maxSide = kHeroMaxSide;
        /// The textures' anisotropy (already clamped to the device's limit).
        float anisotropy = 1.0f;
    };

    /// The pool's priority of a hero job: above every layer's (the focused item's included).
    static constexpr std::int64_t kJobPriority = 1000;

    HeroTextures(Options options, core::WorkerPool& pool);
    /// Cancels its jobs (their pictures go nowhere).
    ~HeroTextures();
    HeroTextures(const HeroTextures&) = delete;
    HeroTextures& operator=(const HeroTextures&) = delete;

    /// Asks for the hero texture of `item` (its cover under the media root): a job on the pool unless it is held,
    /// loading, failed before, the item has no cover or the media root is unknown. A held one is touched.
    void request(const CoverItem& item);
    /// The media root became known (a retry after the catalog's refresh): items asked before may be asked again.
    void setMediaRoot(std::optional<std::filesystem::path> mediaRoot);
    /// One round (main thread): at most one finished picture uploaded, the set kept to its capacity (`pinned`, the
    /// shown one, never evicted); nothing while `paused`. Returns the main thread's milliseconds of the upload (0 when
    /// none went up).
    double update(bool paused, std::span<const std::string> pinned);

    /// Forgets `game`'s texture (its cover changed: I3 step 15b's live update), a picture on its way included; the
    /// next request() asks again. The caller stops showing it first.
    void forget(const catalog::GameId& game);
    /// The resident hero texture of `game`, if any.
    [[nodiscard]] const gl::Texture2D* find(const catalog::GameId& game) const;
    /// A job for `game` runs or its picture waits for its upload.
    [[nodiscard]] bool loading(const catalog::GameId& game) const;
    /// Bytes of the resident textures (what the ledger records under HeroTextures for them).
    [[nodiscard]] std::size_t bytes() const noexcept;
    [[nodiscard]] std::size_t resident() const noexcept { return m_textures.size(); }

    /// The run report's hero record.
    void record(HeroRecord& record) const;

    /// What the jobs and the set share (hero_textures.cpp).
    struct Shared;

private:
    Options m_options;
    core::WorkerPool& m_pool;
    core::JobGeneration m_generation;
    std::shared_ptr<Shared> m_shared;
    HeroLru m_lru;
    std::map<std::string, gl::Texture2D, std::less<>> m_textures;
    /// Games whose job runs or whose picture waits; games that failed (not asked again until the media root changes).
    std::set<std::string, std::less<>> m_loading;
    std::map<std::string, std::string, std::less<>> m_failed;
    /// Games forgotten while their job ran: their pictures are dropped when they come.
    std::set<std::string, std::less<>> m_discarded;
    std::uint64_t m_requests = 0;
    std::uint64_t m_loaded = 0;
    std::uint64_t m_evicted = 0;
    double m_maxUploadMs = 0.0;
    std::uint64_t m_peakBytes = 0;
};

} // namespace rv::app
