#include <rv/core/worker_pool.hpp>

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <map>
#include <mutex>
#include <system_error>
#include <tuple>
#include <utility>

#include <windows.h>

#include <spdlog/spdlog.h>

#include <rv/core/system_error.hpp>
#include <rv/core/utf8.hpp>

namespace rv::core {

std::size_t defaultWorkerThreads(unsigned hardwareThreads) noexcept {
    if (hardwareThreads <= 4) {
        return 1;
    }
    return std::min<std::size_t>(6, hardwareThreads - 3);
}

std::size_t defaultWorkerThreads() noexcept {
    return defaultWorkerThreads(std::thread::hardware_concurrency());
}

namespace {

/// The queue's order: priority from the highest, then submission order.
struct QueueKey {
    std::int64_t priority = 0;
    std::uint64_t sequence = 0;

    bool operator<(const QueueKey& other) const noexcept {
        return std::tie(other.priority, sequence) < std::tie(priority, other.sequence);
    }
};

struct QueuedJob {
    Job job;
    std::shared_ptr<std::atomic<std::uint64_t>> counter; ///< the job's generation counter; null without one
    std::uint64_t generation = 0;

    [[nodiscard]] bool stale() const noexcept { return counter != nullptr && counter->load() != generation; }
};

} // namespace

/// Shared by the pool and its threads: a detached thread keeps it alive after the pool is gone.
struct WorkerPool::State {
    std::string name;
    std::size_t capacity = 1;
    bool belowNormalPriority = true;

    mutable std::mutex mutex;
    std::condition_variable work;         ///< a job queued, or stopping
    mutable std::condition_variable idle; ///< the queue empty and no job running
    std::condition_variable ended;        ///< a thread ended
    std::map<QueueKey, QueuedJob> queue;
    std::uint64_t nextSequence = 0;
    std::size_t running = 0;
    std::vector<bool> threadEnded;     ///< per thread, set as it returns
    std::atomic<bool> stopping{false}; ///< read by JobContext::cancelled() without the mutex

    [[nodiscard]] bool isIdle() const noexcept { return queue.empty() && running == 0; }
};

namespace {

/// The worker's priority and description, set from the thread itself.
void prepareThread(const std::string& name, std::size_t index, bool belowNormal) {
    if (belowNormal && SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL) == FALSE) {
        spdlog::warn("Worker pool '{}': thread {} keeps its normal priority: {}", name, index + 1,
                     lastSystemErrorMessage());
    }
    // Without the scheduler's dynamic boosts (I3 step 12): a worker woken by its file reads would otherwise be
    // lifted to the main thread's priority and share its core in turns, which made the frames of a cold cover load
    // about four times slower on the reference machine (coverLoad p50 40 ms, 11 ms without the boosts).
    if (belowNormal && SetThreadPriorityBoost(GetCurrentThread(), TRUE) == FALSE) {
        spdlog::warn("Worker pool '{}': thread {} keeps its priority boosts: {}", name, index + 1,
                     lastSystemErrorMessage());
    }
    if (const auto description = utf8ToWide(name + " " + std::to_string(index + 1))) {
        // Only for debuggers and profilers: a failure changes nothing else.
        static_cast<void>(SetThreadDescription(GetCurrentThread(), description->c_str()));
    }
}

} // namespace

WorkerPool::WorkerPool(WorkerPoolOptions options) : m_state(std::make_shared<State>()) {
    m_state->name = std::move(options.name);
    m_state->capacity = std::max<std::size_t>(options.capacity, 1);
    m_state->belowNormalPriority = options.belowNormalPriority;
    const std::size_t wanted = options.threads == 0 ? defaultWorkerThreads() : options.threads;
    m_state->threadEnded.assign(wanted, false);
    m_threads.reserve(wanted);
    for (std::size_t index = 0; index < wanted; ++index) {
        try {
            m_threads.emplace_back([state = m_state, index] { workerLoop(state, index); });
        } catch (const std::system_error& e) {
            spdlog::error("Worker pool '{}': cannot start thread {}: {}", m_state->name, index + 1, e.what());
            break;
        }
    }
    m_state->threadEnded.resize(m_threads.size());
    spdlog::debug("Worker pool '{}': {} thread(s), queue of {} job(s){}", m_state->name, m_threads.size(),
                  m_state->capacity, m_state->belowNormalPriority ? ", below-normal priority" : "");
}

WorkerPool::~WorkerPool() {
    shutdown(kShutdownBound);
}

void WorkerPool::workerLoop(const std::shared_ptr<State>& state, std::size_t index) {
    prepareThread(state->name, index, state->belowNormalPriority);
    std::unique_lock lock(state->mutex);
    while (true) {
        state->work.wait(lock, [&] { return state->stopping.load() || !state->queue.empty(); });
        if (state->stopping.load()) {
            break;
        }
        auto first = state->queue.begin();
        QueuedJob queued = std::move(first->second);
        state->queue.erase(first);
        if (queued.stale()) {
            // Cancelled while queued: dropped without running (its function destroyed outside the lock).
            const bool nowIdle = state->isIdle();
            lock.unlock();
            queued = {};
            if (nowIdle) {
                state->idle.notify_all();
            }
            lock.lock();
            continue;
        }
        ++state->running;
        lock.unlock();
        try {
            queued.job(JobContext(&state->stopping, queued.counter.get(), queued.generation));
        } catch (const std::exception& e) {
            spdlog::error("Worker pool '{}': a job failed: {}", state->name, e.what());
        } catch (...) {
            spdlog::error("Worker pool '{}': a job failed with an unknown exception", state->name);
        }
        queued = {};
        lock.lock();
        --state->running;
        if (state->isIdle()) {
            state->idle.notify_all();
        }
    }
    state->threadEnded[index] = true;
    lock.unlock();
    state->ended.notify_all();
}

Status WorkerPool::submit(Job job, const JobOptions& options) {
    if (!job) {
        return makeError(ErrorCode::InvalidArgument, "an empty job", m_state->name);
    }
    QueuedJob queued{std::move(job), nullptr, 0};
    if (options.generation != nullptr) {
        queued.counter = options.generation->m_counter;
        queued.generation = queued.counter->load();
    }
    {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->stopping.load()) {
            return makeError(ErrorCode::Cancelled, "the worker pool is shut down", m_state->name);
        }
        // The threads' flags, not m_threads: a job may submit from a worker thread while the owner shuts down.
        if (m_state->threadEnded.empty()) {
            return makeError(ErrorCode::Unavailable, "the worker pool has no thread", m_state->name);
        }
        if (m_state->queue.size() >= m_state->capacity) {
            return makeError(ErrorCode::Unavailable,
                             "the worker pool's queue is full (" + std::to_string(m_state->capacity) + " jobs)",
                             m_state->name);
        }
        m_state->queue.emplace(QueueKey{options.priority, m_state->nextSequence++}, std::move(queued));
    }
    m_state->work.notify_one();
    return ok();
}

std::size_t WorkerPool::cancel(JobGeneration& generation) {
    generation.advance();
    std::vector<QueuedJob> dropped;
    bool nowIdle = false;
    {
        const std::lock_guard lock(m_state->mutex);
        for (auto it = m_state->queue.begin(); it != m_state->queue.end();) {
            if (it->second.counter == generation.m_counter && it->second.stale()) {
                dropped.push_back(std::move(it->second));
                it = m_state->queue.erase(it);
            } else {
                ++it;
            }
        }
        nowIdle = !dropped.empty() && m_state->isIdle();
    }
    if (nowIdle) {
        m_state->idle.notify_all();
    }
    return dropped.size(); // their functions are destroyed here, outside the lock
}

bool WorkerPool::waitIdle(std::chrono::milliseconds bound) const {
    std::unique_lock lock(m_state->mutex);
    return m_state->idle.wait_for(lock, bound, [this] { return m_state->isIdle(); });
}

bool WorkerPool::shutdown(std::chrono::milliseconds bound) {
    if (m_threads.empty()) {
        m_state->stopping = true;
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + bound;
    std::map<QueueKey, QueuedJob> dropped;
    {
        const std::lock_guard lock(m_state->mutex);
        m_state->stopping = true;
        dropped.swap(m_state->queue);
    }
    m_state->work.notify_all();
    m_state->idle.notify_all();
    if (!dropped.empty()) {
        spdlog::debug("Worker pool '{}': {} queued job(s) dropped at shutdown", m_state->name, dropped.size());
        dropped.clear();
    }

    std::vector<bool> ended;
    {
        std::unique_lock lock(m_state->mutex);
        m_state->ended.wait_until(lock, deadline, [this] {
            return std::all_of(m_state->threadEnded.begin(), m_state->threadEnded.end(),
                               [](bool done) { return done; });
        });
        ended = m_state->threadEnded;
    }
    std::size_t detached = 0;
    for (std::size_t index = 0; index < m_threads.size(); ++index) {
        if (ended[index]) {
            m_threads[index].join(); // returned already: at once
        } else {
            m_threads[index].detach();
            ++detached;
        }
    }
    m_threads.clear();
    if (detached != 0) {
        spdlog::warn("Worker pool '{}': {} thread(s) still running a job after {} ms are left to end by themselves",
                     m_state->name, detached, bound.count());
        return false;
    }
    return true;
}

std::size_t WorkerPool::queued() const {
    const std::lock_guard lock(m_state->mutex);
    return m_state->queue.size();
}

} // namespace rv::core
