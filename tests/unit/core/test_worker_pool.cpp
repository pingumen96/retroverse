// The worker pool (rv/core/worker_pool.hpp, ADR-0011 §6): its thread count, priorities, bound, cancellation by
// generation, shutdown bound and thread priority. Jobs wait on gates the test opens, never on sleeps alone, so that the
// order they run in is the pool's and not the scheduler's.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <windows.h>

#include "support/log_capture.hpp"
#include <rv/core/worker_pool.hpp>

using rv::core::Job;
using rv::core::JobContext;
using rv::core::JobGeneration;
using rv::core::JobOptions;
using rv::core::WorkerPool;
using rv::core::WorkerPoolOptions;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

/// A gate jobs wait at until the test opens it.
class Gate {
public:
    Gate() : m_opened(m_promise.get_future().share()) {}
    void open() { m_promise.set_value(); }
    /// Waits for the gate, at most `bound`. True when it opened.
    [[nodiscard]] bool wait(std::chrono::milliseconds bound = 10s) const {
        return m_opened.wait_for(bound) == std::future_status::ready;
    }

private:
    std::promise<void> m_promise;
    std::shared_future<void> m_opened;
};

/// Records what jobs did, in order.
class Journal {
public:
    void add(int value) {
        const std::lock_guard lock(m_mutex);
        m_values.push_back(value);
    }
    [[nodiscard]] std::vector<int> values() const {
        const std::lock_guard lock(m_mutex);
        return m_values;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<int> m_values;
};

/// A pool for the tests: its threads at normal priority, so that a machine busy with other work (a build, other test
/// runs) cannot starve them for seconds and stretch the timings (below-normal threads wait for every normal one;
/// Windows lifts a starved thread only after about 4 s). The priority itself has a test of its own.
WorkerPoolOptions testPool(std::size_t threads, std::size_t capacity = 4096) {
    WorkerPoolOptions options;
    options.threads = threads;
    options.capacity = capacity;
    options.belowNormalPriority = false;
    options.name = "rv-test-pool";
    return options;
}

WorkerPoolOptions oneThread(std::size_t capacity = 4096) {
    return testPool(1, capacity);
}

/// Occupies the pool's only thread until `release` opens; returns once the job has started.
void block(WorkerPool& pool, const Gate& release) {
    auto started = std::make_shared<Gate>();
    REQUIRE(pool.submit([started, &release](const JobContext&) {
                    started->open();
                    static_cast<void>(release.wait());
                })
                .has_value());
    REQUIRE(started->wait());
}

} // namespace

TEST_CASE("core/worker_pool: min(6, hardware threads - 3) threads, at least one", "[core][worker_pool]") {
    CHECK(rv::core::defaultWorkerThreads(0) == 1); // unknown
    CHECK(rv::core::defaultWorkerThreads(1) == 1);
    CHECK(rv::core::defaultWorkerThreads(2) == 1);
    CHECK(rv::core::defaultWorkerThreads(4) == 1);
    CHECK(rv::core::defaultWorkerThreads(5) == 2);
    CHECK(rv::core::defaultWorkerThreads(8) == 5); // the reference machine
    CHECK(rv::core::defaultWorkerThreads(9) == 6);
    CHECK(rv::core::defaultWorkerThreads(64) == 6);
    CHECK(rv::core::defaultWorkerThreads() == rv::core::defaultWorkerThreads(std::thread::hardware_concurrency()));

    WorkerPool pool(testPool(0)); // 0: the default count
    CHECK(pool.threadCount() == rv::core::defaultWorkerThreads());
    CHECK(WorkerPoolOptions{}.threads == 0);
    CHECK(WorkerPoolOptions{}.belowNormalPriority);
}

TEST_CASE("core/worker_pool: higher priorities first, equal ones in submission order", "[core][worker_pool]") {
    WorkerPool pool(oneThread());
    Gate release;
    block(pool, release);
    Journal journal;
    const auto record = [&journal](int id) {
        return [&journal, id](const JobContext&) {
            journal.add(id);
        };
    };
    const std::int64_t priorities[] = {0, 5, 0, 10, 5, -3, WorkerPool::kHighestPriority, -3};
    for (int id = 0; id < static_cast<int>(std::size(priorities)); ++id) {
        REQUIRE(pool.submit(record(id), JobOptions{priorities[id]}).has_value());
    }
    CHECK(pool.queued() == std::size(priorities));
    CHECK_FALSE(pool.waitIdle(20ms)); // the blocker runs
    release.open();
    REQUIRE(pool.waitIdle(10s));
    CHECK(journal.values() == std::vector<int>{6, 3, 1, 4, 0, 2, 5, 7});
    CHECK(pool.queued() == 0);
}

TEST_CASE("core/worker_pool: every job runs once on many threads", "[core][worker_pool]") {
    WorkerPool pool(testPool(4));
    REQUIRE(pool.threadCount() == 4);
    std::atomic<int> runs{0};
    std::mutex threadsMutex;
    std::vector<std::thread::id> threads;
    for (int i = 0; i < 4000; ++i) {
        REQUIRE(pool.submit([&](const JobContext&) {
                        ++runs;
                        const std::lock_guard lock(threadsMutex);
                        threads.push_back(std::this_thread::get_id());
                    })
                    .has_value());
    }
    REQUIRE(pool.waitIdle(10s));
    CHECK(runs == 4000);
    for (const std::thread::id id : threads) {
        CHECK(id != std::this_thread::get_id()); // never on the caller's thread
    }
}

TEST_CASE("core/worker_pool: cancellation by generation", "[core][worker_pool]") {
    WorkerPool pool(oneThread());
    Gate release;
    block(pool, release);
    Journal journal;
    JobGeneration covers;
    JobGeneration plans;
    const auto record = [&journal](int id) {
        return [&journal, id](const JobContext&) {
            journal.add(id);
        };
    };
    for (int id = 0; id < 5; ++id) {
        REQUIRE(pool.submit(record(id), JobOptions{0, &covers}).has_value());
    }
    REQUIRE(pool.submit(record(10), JobOptions{0, &plans}).has_value());
    REQUIRE(pool.submit(record(20)).has_value());

    // The covers' jobs leave the queue at once; the others stay.
    CHECK(pool.cancel(covers) == 5);
    CHECK(covers.current() == 1);
    CHECK(pool.queued() == 2);
    // A job of the new generation runs.
    REQUIRE(pool.submit(record(5), JobOptions{0, &covers}).has_value());
    // A generation advanced without cancel(): its queued jobs are dropped when their turn comes, never run.
    REQUIRE(pool.submit(record(11), JobOptions{0, &plans}).has_value());
    const JobGeneration sharedPlans = plans; // copies share the counter
    CHECK(plans.advance() == 1);
    CHECK(sharedPlans.current() == 1);

    release.open();
    REQUIRE(pool.waitIdle(10s));
    CHECK(journal.values() == std::vector<int>{20, 5});
    CHECK(pool.cancel(covers) == 0); // nothing queued
}

TEST_CASE("core/worker_pool: a running job sees its cancellation", "[core][worker_pool]") {
    WorkerPool pool(oneThread());
    JobGeneration generation;
    Gate started;
    std::atomic<bool> sawCancelled{false};
    std::atomic<std::uint64_t> generationSeen{99};
    REQUIRE(pool.submit(
                    [&](const JobContext& context) {
                        generationSeen = context.generation();
                        started.open();
                        const auto deadline = Clock::now() + 10s;
                        while (!context.cancelled() && Clock::now() < deadline) {
                            std::this_thread::sleep_for(1ms);
                        }
                        sawCancelled = context.cancelled();
                    },
                    JobOptions{0, &generation})
                .has_value());
    REQUIRE(started.wait());
    CHECK(pool.cancel(generation) == 0); // running, not queued
    REQUIRE(pool.waitIdle(5s));
    CHECK(sawCancelled);
    CHECK(generationSeen == 0);
    CHECK(generation.current() == 1); // a result tagged 0 is stale now

    // A context made without a pool never is.
    const JobContext alone;
    CHECK_FALSE(alone.cancelled());
    CHECK(alone.generation() == 0);
}

TEST_CASE("core/worker_pool: the queue is bounded", "[core][worker_pool]") {
    WorkerPool pool(oneThread(3));
    Gate release;
    block(pool, release); // running: not in the queue
    JobGeneration generation;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(pool.submit([](const JobContext&) {}, JobOptions{0, &generation}).has_value());
    }
    const auto full = pool.submit([](const JobContext&) {});
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().code == rv::ErrorCode::Unavailable);
    CHECK(full.error().message.find("full") != std::string::npos);
    // Cancelling frees the room at once.
    CHECK(pool.cancel(generation) == 3);
    CHECK(pool.submit([](const JobContext&) {}).has_value());
    release.open();
    REQUIRE(pool.waitIdle(10s));

    const auto empty = pool.submit(Job{});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("core/worker_pool: a shutdown with jobs queued ends within 2 s", "[core][worker_pool]") {
    WorkerPool pool(testPool(2));
    // Both threads busy with long jobs that stop when cancelled, then a thousand jobs queued behind them.
    std::atomic<int> started{0};
    std::atomic<int> stoppedEarly{0};
    for (int i = 0; i < 2; ++i) {
        REQUIRE(pool.submit([&](const JobContext& context) {
                        ++started;
                        const auto deadline = Clock::now() + 30s;
                        while (!context.cancelled() && Clock::now() < deadline) {
                            std::this_thread::sleep_for(1ms);
                        }
                        stoppedEarly += context.cancelled() ? 1 : 0;
                    })
                    .has_value());
    }
    const auto startBy = Clock::now() + 10s;
    while (started < 2 && Clock::now() < startBy) {
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(started == 2);
    std::atomic<int> queuedRuns{0};
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(pool.submit([&](const JobContext&) { ++queuedRuns; }, JobOptions{WorkerPool::kHighestPriority})
                    .has_value());
    }

    const auto start = Clock::now();
    CHECK(pool.shutdown());
    CHECK(Clock::now() - start < 2s);
    CHECK(stoppedEarly == 2);
    CHECK(queuedRuns == 0); // dropped, never run
    CHECK(pool.queued() == 0);
    CHECK(pool.threadCount() == 0);

    const auto late = pool.submit([](const JobContext&) {});
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error().code == rv::ErrorCode::Cancelled);
    CHECK(pool.shutdown()); // again: nothing to do
}

TEST_CASE("core/worker_pool: a job that ignores its cancellation is left to end by itself", "[core][worker_pool]") {
    rv::test::LogCapture log;
    auto ended = std::make_shared<std::atomic<bool>>(false);
    auto started = std::make_shared<Gate>();
    {
        WorkerPool pool(oneThread());
        REQUIRE(pool.submit([ended, started](const JobContext&) {
                        started->open();
                        std::this_thread::sleep_for(600ms);
                        *ended = true;
                    })
                    .has_value());
        REQUIRE(started->wait());
        const auto start = Clock::now();
        CHECK_FALSE(pool.shutdown(100ms));
        CHECK(Clock::now() - start < 500ms);
        CHECK_FALSE(*ended);
    } // the pool is gone; its thread is not
    CHECK(log.count("left to end by themselves") == 1);
    const auto deadline = Clock::now() + 10s;
    while (!*ended && Clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(*ended);
}

TEST_CASE("core/worker_pool: threads run below normal priority", "[core][worker_pool]") {
    const auto prioritiesOf = [](bool belowNormal) {
        WorkerPoolOptions options;
        options.threads = 2;
        options.belowNormalPriority = belowNormal;
        WorkerPool pool(options);
        std::mutex mutex;
        std::vector<int> priorities;
        for (int i = 0; i < 8; ++i) {
            REQUIRE(pool.submit([&](const JobContext&) {
                            const int priority = GetThreadPriority(GetCurrentThread());
                            const std::lock_guard lock(mutex);
                            priorities.push_back(priority);
                        })
                        .has_value());
        }
        REQUIRE(pool.waitIdle(30s)); // below normal: a busy machine may starve them for seconds
        CHECK(priorities.size() == 8);
        return priorities;
    };
    for (const int priority : prioritiesOf(true)) {
        CHECK(priority == THREAD_PRIORITY_BELOW_NORMAL);
    }
    for (const int priority : prioritiesOf(false)) {
        CHECK(priority == THREAD_PRIORITY_NORMAL);
    }
}

TEST_CASE("core/worker_pool: an exception a job lets out is logged and goes no further", "[core][worker_pool]") {
    rv::test::LogCapture log;
    WorkerPool pool(oneThread());
    std::atomic<bool> nextRan{false};
    REQUIRE(pool.submit([](const JobContext&) { throw std::runtime_error("decoder exploded"); }).has_value());
    REQUIRE(pool.submit([](const JobContext&) { throw 42; }).has_value());
    REQUIRE(pool.submit([&](const JobContext&) { nextRan = true; }).has_value());
    REQUIRE(pool.waitIdle(10s));
    CHECK(nextRan);
    CHECK(log.count("a job failed: decoder exploded") == 1);
    CHECK(log.count("a job failed with an unknown exception") == 1);
}
