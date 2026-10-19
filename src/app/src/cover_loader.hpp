#pragma once

// The cover loader (ADR-0011 §6, §7; I3 step 10b's first form, completed by step 12): the room's items' fronts and
// spines made on the worker pool through the derived-texture cache (app::serveItemLayers: the cover, or the generated
// label, and the spine on the cover's dominant colour), queued into the collection's arrays and uploaded on the main
// thread within the per-frame budget (gl::CollectionArrays::uploadPending: 1 ms or 64 layers), each upload naming its
// layer in the item's record (CollectionInstance::layerResident): until then the item shows the pending picture.
//
// The order is app::CoverSchedule's (cover_requests.hpp): the focused item first, then the items in view by their
// projected size, then the rest by distance from the walker, ranked again whenever the view changes; it decides both
// which items are made next (a bounded number of jobs at a time) and which made layers go up next. While a launched
// game runs nothing is made or uploaded (the running jobs finish, their layers wait). With the media root unknown the
// cache alone serves the covers (a cover it lacks shows its generated label); retry() checks the media root again
// (after the catalog's refresh) and, once it can be read, serves again every item whose front did not come from its
// cover, uploading only the pictures that changed. Once every layer is resident the cache is trimmed on a worker to its
// bound, never an entry this room uses. Main thread only, but for its jobs.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rv/app/collection_layers.hpp>
#include <rv/app/cover_requests.hpp>
#include <rv/app/run_report.hpp>
#include <rv/core/aabb.hpp>
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

    /// One round's circumstances.
    struct Round {
        /// The frame's view (the priorities, the lockstep's view).
        CoverView view;
        /// The focused (pulled-out, inspected) item, ranked first.
        std::optional<std::size_t> focused;
        /// A launched game runs: nothing is made or uploaded (ADR-0012 §6).
        bool paused = false;
        gl::UploadBudget budget;
        /// Main-thread time already spent on uploads this round (the hero texture's), counted against the budget.
        std::chrono::nanoseconds spent{0};
        /// A frame's round (its uploads count for the budget's records), not an idle wake's or a lockstep wait's.
        bool frame = true;
        /// The frame it serves (the run report's).
        std::uint64_t frameIndex = 0;
    };

    /// Starts serving `items` (the collection's item order) whose boxes in the room are `bounds` (one per item).
    CoverLoader(Options options, core::WorkerPool& pool, std::vector<CoverItem> items, std::vector<core::Aabb> bounds);
    /// Cancels its jobs (their results go nowhere).
    ~CoverLoader();
    CoverLoader(const CoverLoader&) = delete;
    CoverLoader& operator=(const CoverLoader&) = delete;

    /// One round, every frame (and every idle wake or wait of a lockstep run): the finished layers taken, the made ones
    /// submitted to `collection`'s arrays in the schedule's order, the queue uploaded within the budget, each upload
    /// written into its item's record, more jobs submitted. Errors: those of the record writes.
    [[nodiscard]] Status update(CollectionInstance& collection, const Round& round);

    /// Every item's layers are resident (or could not be made), and no retry is on its way.
    [[nodiscard]] bool done() const noexcept;
    /// Every item in the last round's view (and the focused one) is done.
    [[nodiscard]] bool viewDone() const noexcept { return m_schedule.viewDone(); }
    /// Blocks until a job posts a result, at most `timeout` (a lockstep wait within a frame).
    void waitForResults(std::chrono::milliseconds timeout) const;

    /// Takes over what `previous` (a loader of the same collection, now replaced) showed: each item of the same game
    /// that it had made is served again as a retry is, its pictures uploaded only when they changed (I3 step 10c's
    /// textures reconciliation serves the stored room's covers again with the media root).
    void adoptShown(const CoverLoader& previous);
    /// Checks `mediaRoot` on a worker and, when it can be read, serves again every item whose front did not come from
    /// its cover (after the catalog's refresh: ADR-0011 §6). Nothing while a check is on its way.
    void retry(std::filesystem::path mediaRoot);
    /// The media root the covers are read under now (none: unknown).
    [[nodiscard]] const std::optional<std::filesystem::path>& mediaRoot() const noexcept { return m_options.mediaRoot; }
    /// The items it serves (the collection's order).
    [[nodiscard]] const std::vector<CoverItem>& items() const noexcept { return m_items; }

    /// The run report's residency (the arrays' counts from `collection`).
    void record(ResidencyRecord& record, const CollectionInstance& collection) const;
    /// When the last layer became resident (none yet), and when every item of the first view was.
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> allResidentAt() const noexcept {
        return m_allResidentAt;
    }
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> viewResidentAt() const noexcept {
        return m_viewResidentAt;
    }
    /// The Stats panel's lines (its "Collection" section).
    [[nodiscard]] std::vector<std::string> statusLines(const CollectionInstance& collection) const;

    /// What the jobs and the loader share (cover_loader.cpp).
    struct Shared;

private:
    /// Takes what the jobs finished: the schedule told, the layers kept for their upload.
    void takeFinished();
    /// Submits jobs for the schedule's next items while fewer than the bound run and the made layers stay few.
    void submitJobs();
    /// Submits the schedule's next made items to the arrays (up to `maxLayers` layers).
    void submitUploads(CollectionInstance& collection, std::size_t maxLayers);
    void takeRootCheck();
    void startTrim();
    void noteArrivals();

    Options m_options;
    core::WorkerPool& m_pool;
    core::JobGeneration m_generation;
    std::shared_ptr<Shared> m_shared;
    std::vector<CoverItem> m_items;
    CoverSchedule m_schedule;
    /// Per item: the layers made and waiting for their upload, the front's source and the cache entries served for it
    /// (the trim's in-use set; a retry compares them).
    std::vector<std::optional<ItemLayers>> m_made;
    std::vector<std::optional<FrontSource>> m_front;
    std::vector<std::string> m_frontEntry;
    std::vector<std::string> m_spineEntry;
    std::vector<std::uint8_t> m_retrying;
    std::size_t m_failedLayers = 0;
    bool m_rootCheckPending = false;
    bool m_trimStarted = false;
    bool m_wasDone = false;
    std::uint64_t m_uploadRounds = 0;
    std::uint64_t m_maxLayersPerRound = 0;
    std::uint64_t m_overBudgetRounds = 0;
    double m_maxRoundMs = 0.0;
    std::uint64_t m_pausedRounds = 0;
    std::uint64_t m_retryRounds = 0;
    std::uint64_t m_retriedItems = 0;
    std::uint64_t m_retriedChanged = 0;
    std::uint64_t m_adopted = 0;
    std::uint64_t m_adoptedChanged = 0;
    /// The first view's items, and when they were all resident (the arrival record).
    std::optional<std::size_t> m_viewItems;
    std::size_t m_residentAtViewResident = 0;
    std::optional<std::chrono::system_clock::time_point> m_viewResidentAt;
    std::optional<std::chrono::system_clock::time_point> m_allResidentAt;
    std::optional<std::uint64_t> m_allResidentFrame;
};

} // namespace rv::app
