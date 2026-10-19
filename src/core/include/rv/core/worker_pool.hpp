#pragma once

// The worker pool (ADR-0011 §6, ADR-0010 §12): a few threads at below-normal priority that decode and prepare the
// room's images and run the world's generation job, the latter at the highest priority. Bounded (a full queue refuses a
// job), ordered by priority, with jobs of one kind cancelled together by advancing their generation, and stopped within
// a bound at shutdown. Jobs post their results themselves (to the main thread, which commits them at a frame boundary).

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::core {

/// The generation of one kind of jobs (the covers of the room shown, the world's plans): a job submitted under it
/// belongs to the generation current then, and advancing it makes every job of earlier generations stale: queued, it
/// never starts; running, it sees JobContext::cancelled() and its result must be dropped. Copies share one counter.
/// Thread-safe.
class JobGeneration {
public:
    JobGeneration() : m_counter(std::make_shared<std::atomic<std::uint64_t>>(0)) {}

    /// The generation now (0 at first).
    [[nodiscard]] std::uint64_t current() const noexcept { return m_counter->load(); }
    /// Makes the jobs submitted so far stale; returns the new generation. WorkerPool::cancel() also drops them from the
    /// queue at once.
    std::uint64_t advance() noexcept { return m_counter->fetch_add(1) + 1; }

private:
    friend class WorkerPool;
    std::shared_ptr<std::atomic<std::uint64_t>> m_counter;
};

/// What a running job sees of its own state.
class JobContext {
public:
    /// A context that is never cancelled, for calling a job's function without a pool (tests).
    JobContext() noexcept = default;

    /// True once the job's generation was advanced or the pool is shutting down: the job should return early, and
    /// nothing it produced may be used.
    [[nodiscard]] bool cancelled() const noexcept {
        return (m_stopping != nullptr && m_stopping->load()) ||
               (m_counter != nullptr && m_counter->load() != m_generation);
    }
    /// The generation the job was submitted under (0 without one): a result carries it, so that the main thread drops
    /// one that arrives after JobGeneration::advance().
    [[nodiscard]] std::uint64_t generation() const noexcept { return m_generation; }

private:
    friend class WorkerPool;
    JobContext(const std::atomic<bool>* stopping, const std::atomic<std::uint64_t>* counter,
               std::uint64_t generation) noexcept
        : m_stopping(stopping), m_counter(counter), m_generation(generation) {}

    const std::atomic<bool>* m_stopping = nullptr;
    const std::atomic<std::uint64_t>* m_counter = nullptr;
    std::uint64_t m_generation = 0;
};

/// A job: runs on a worker thread, never on the main thread. An exception it lets out is logged and goes no further.
using Job = std::function<void(const JobContext&)>;

/// How a job is queued.
struct JobOptions {
    /// Higher runs first; equal priorities run in submission order.
    std::int64_t priority = 0;
    /// The kind of jobs it belongs to (copied at submission); none: only the pool's shutdown cancels it.
    const JobGeneration* generation = nullptr;
};

/// The pool's threads: min(6, hardware threads - 3), at least 1, which leaves the main thread, the catalog worker and
/// the video decoder theirs (5 on the reference machine's 8 hardware threads). 0 hardware threads (unknown) gives 1.
[[nodiscard]] std::size_t defaultWorkerThreads(unsigned hardwareThreads) noexcept;
/// defaultWorkerThreads() of std::thread::hardware_concurrency().
[[nodiscard]] std::size_t defaultWorkerThreads() noexcept;

struct WorkerPoolOptions {
    /// The threads; 0: defaultWorkerThreads().
    std::size_t threads = 0;
    /// The jobs the queue holds at most (stale ones included until they are dropped); 0 is taken as 1.
    std::size_t capacity = 4096;
    /// THREAD_PRIORITY_BELOW_NORMAL for every thread (ADR-0011 §6), without the scheduler's dynamic boosts (I3 step
    /// 12: SetThreadPriorityBoost), so that the main thread and the video decoder always come first.
    bool belowNormalPriority = true;
    /// The threads' description ("<name> <n>", seen in debuggers and profilers) and the logs' name of the pool.
    std::string name = "rv-worker";
};

class WorkerPool {
public:
    /// How long the destructor waits for running jobs to return.
    static constexpr std::chrono::milliseconds kShutdownBound{2000};
    /// The world's generation job (ADR-0010 §12).
    static constexpr std::int64_t kHighestPriority = std::numeric_limits<std::int64_t>::max();

    /// Starts the threads. A thread the system cannot start is logged and left out; with none, every submission is
    /// refused.
    explicit WorkerPool(WorkerPoolOptions options = {});
    /// shutdown(kShutdownBound) unless shut down already.
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /// Queues `job`. Errors: InvalidArgument (an empty job), Unavailable (the queue holds its capacity: try again
    /// later, or cancel stale work first; also with no thread), Cancelled (the pool is shut down).
    [[nodiscard]] Status submit(Job job, const JobOptions& options = {});

    /// Advances `generation` and drops at once every queued job of it (they never start; their functions are
    /// destroyed on this thread). Running jobs of it see cancelled(). Returns the jobs dropped.
    std::size_t cancel(JobGeneration& generation);

    /// Waits until no job is queued or running, at most `bound`. True when the pool became idle. Never from a job,
    /// which counts as running until it returns.
    [[nodiscard]] bool waitIdle(std::chrono::milliseconds bound) const;

    /// Stops the pool: no job is accepted any more, the queued ones are dropped without running, running jobs see
    /// cancelled(), and the threads are joined as they end, for at most `bound` in all. A thread whose job is still
    /// running then is detached and logged (it ends by itself; the pool's shared state outlives it): returns false.
    /// True when every thread ended in time, or the pool was shut down already. Called by the pool's owner (never from
    /// a job, nor from two threads at once).
    bool shutdown(std::chrono::milliseconds bound = kShutdownBound);

    /// The threads started.
    [[nodiscard]] std::size_t threadCount() const noexcept { return m_threads.size(); }
    /// The jobs queued and not started (stale ones included until a worker drops them).
    [[nodiscard]] std::size_t queued() const;

private:
    struct State;

    /// A thread's life: jobs taken in queue order until the pool stops.
    static void workerLoop(const std::shared_ptr<State>& state, std::size_t index);

    std::shared_ptr<State> m_state;
    std::vector<std::thread> m_threads;
};

} // namespace rv::core
