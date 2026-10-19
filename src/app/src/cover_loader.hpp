#pragma once

// The cover loader (ADR-0011 §6, §7; I3 step 10b's first form, step 12 orders and extends it): the room's items' fronts
// and spines made on the worker pool through the derived-texture cache (app::serveItemLayers: the cover, or the
// generated label, and the spine), queued into the collection's arrays and uploaded on the main thread within the
// per-frame budget (gl::CollectionArrays::uploadPending: 1 ms or 64 layers), each upload naming its layer in the item's
// record (CollectionInstance::layerResident): until then the item shows the pending picture. Items are served in the
// layout's order (step 12: by visibility and distance), a bounded number of jobs at a time. Once every layer is
// resident the cache is trimmed on a worker to its bound, never an entry this room uses. Main thread only, but for its
// jobs.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/collection_layers.hpp>
#include <rv/app/run_report.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/worker_pool.hpp>
#include <rv/render/texture_array_layout.hpp>

namespace rv::app {

class CollectionInstance;

class CoverLoader {
public:
    struct Options {
        /// The asset root (the bundled fonts) and the library's cache folder (assets::textureCacheRoot).
        std::filesystem::path assetRoot;
        std::filesystem::path cacheRoot;
        gl::CollectionArrayFormat format = gl::CollectionArrayFormat::Bc1Srgb;
        /// The covers are read under it; none while the media are unknown (the cache alone serves them).
        std::optional<std::filesystem::path> mediaRoot;
        /// Called by a job once its result is posted (AppContext::wakeCallback(): thread-safe).
        std::function<void()> wake;
        /// Jobs on the pool at a time (0: twice the pool's threads).
        std::size_t jobsInFlight = 0;
    };

    /// Starts serving `items` (in their order, the collection's item order).
    CoverLoader(Options options, core::WorkerPool& pool, std::vector<CoverItem> items);
    /// Cancels its jobs (their results go nowhere).
    ~CoverLoader();
    CoverLoader(const CoverLoader&) = delete;
    CoverLoader& operator=(const CoverLoader&) = delete;

    /// One round, every frame (and every idle wake of a lockstep run): the finished layers submitted to `collection`'s
    /// arrays, the queue uploaded within `budget`, each upload written into its item's record, more jobs submitted.
    /// `frame` is the frame it serves (the run report's). Errors: those of the record writes.
    [[nodiscard]] Status update(CollectionInstance& collection, const gl::UploadBudget& budget, std::uint64_t frame);

    /// Every item's layers are resident, or could not be made.
    [[nodiscard]] bool done() const noexcept { return m_done; }
    /// The run report's residency (the arrays' counts from `collection`).
    void record(ResidencyRecord& record, const CollectionInstance& collection) const;
    /// When the last layer became resident (none yet).
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> allResidentAt() const noexcept {
        return m_allResidentAt;
    }

    /// What the jobs and the loader share (cover_loader.cpp).
    struct Shared;

private:
    /// Submits jobs while fewer than the bound are in flight and the made layers waiting for their upload
    /// (`queuedUploads` in the arrays, two per job in flight) stay few.
    void submitJobs(std::size_t queuedUploads);
    void startTrim();

    Options m_options;
    core::WorkerPool& m_pool;
    core::JobGeneration m_generation;
    std::shared_ptr<Shared> m_shared;
    std::vector<CoverItem> m_items;
    std::size_t m_next = 0;
    std::size_t m_inFlight = 0;
    std::size_t m_processed = 0;
    std::size_t m_failedLayers = 0;
    std::size_t m_covers = 0;
    std::size_t m_cachedCovers = 0;
    std::size_t m_labels = 0;
    std::size_t m_coverFailures = 0;
    std::vector<std::string> m_entries;
    bool m_done = false;
    bool m_trimStarted = false;
    std::uint64_t m_uploadRounds = 0;
    std::uint64_t m_maxLayersPerRound = 0;
    std::optional<std::chrono::system_clock::time_point> m_allResidentAt;
    std::optional<std::uint64_t> m_allResidentFrame;
};

} // namespace rv::app
