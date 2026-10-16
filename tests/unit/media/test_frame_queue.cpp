#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include "media_test_support.hpp"
#include <rv/media/frame_queue.hpp>
#include <rv/media/video_frame.hpp>

using rv::media::FramePool;
using rv::media::FrameQueue;
using rv::media::FrameSize;
using rv::media::FrameTiming;
using rv::media::PooledFrame;
using rv::media::VideoFrame;

namespace {

/// Takes a frame from `pool` stamped with `pts`; empty when none is free. No assertion: also called on other threads.
PooledFrame stamped(FramePool& pool, double pts) {
    PooledFrame frame = pool.tryAcquire();
    if (frame) {
        frame->timing = FrameTiming{pts, 0.1, 0};
    }
    return frame;
}

} // namespace

TEST_CASE("media/frame_pool: frames are reused and come back when their handle goes", "[media][frame_pool]") {
    const auto pool = FramePool::create(2);
    CHECK(pool->capacity() == 2);
    CHECK(pool->available() == 2);
    PooledFrame first = pool->tryAcquire();
    PooledFrame second = pool->tryAcquire();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first.get() != second.get());
    CHECK(pool->available() == 0);
    CHECK_FALSE(pool->tryAcquire());

    VideoFrame* const reused = first.get();
    first.reset();
    CHECK_FALSE(first);
    CHECK(pool->available() == 1);
    PooledFrame again = pool->tryAcquire();
    CHECK(again.get() == reused);

    PooledFrame moved = std::move(again);
    CHECK_FALSE(again); // NOLINT(bugprone-use-after-move): a moved-from handle is empty
    CHECK(moved.get() == reused);
    moved = PooledFrame{};
    CHECK(pool->available() == 1);
}

TEST_CASE("media/frame_pool: handles keep the pool alive", "[media][frame_pool]") {
    PooledFrame frame;
    {
        const auto pool = FramePool::create(1);
        frame = pool->tryAcquire();
    }
    REQUIRE(frame);
    CHECK(frame->resize(FrameSize{4, 4}));
    frame.reset(); // returns the frame to a pool only the handle still referenced
}

TEST_CASE("media/frame_pool: acquire waits for a frame and wakes on close or stop", "[media][frame_pool]") {
    const auto pool = FramePool::create(1);
    PooledFrame held = pool->tryAcquire();
    REQUIRE(held);

    std::atomic<bool> gotFrame{false};
    std::jthread waiter([&](std::stop_token stop) { gotFrame = static_cast<bool>(pool->acquire(stop)); });
    held.reset();
    REQUIRE(rv::test::waitUntil([&] { return gotFrame.load(); }));
    waiter.join();

    PooledFrame block = pool->tryAcquire(); // the waiter above gave its frame back: hold it so the next ones wait
    REQUIRE(block);
    std::atomic<bool> returned{false};
    std::jthread stopped([&](std::stop_token stop) {
        const PooledFrame none = pool->acquire(stop);
        returned = !none;
    });
    stopped.request_stop();
    stopped.join();
    CHECK(returned);

    std::atomic<bool> closedReturned{false};
    std::jthread closed([&](std::stop_token stop) {
        const PooledFrame none = pool->acquire(stop);
        closedReturned = !none;
    });
    pool->close();
    closed.join();
    CHECK(closedReturned);
    block.reset();
    CHECK_FALSE(pool->tryAcquire()); // closed for good
}

TEST_CASE("media/frame_pool: frames reallocate only when they grow", "[media][frame_pool]") {
    VideoFrame frame;
    CHECK(frame.resize(FrameSize{64, 32}));
    CHECK(frame.size() == FrameSize{64, 32});
    CHECK(frame.strideBytes() == 256);
    CHECK(frame.sizeBytes() == 64u * 32u * 4u);
    CHECK(frame.bytes().size() == frame.sizeBytes());
    CHECK(reinterpret_cast<std::uintptr_t>(frame.pixels()) % 64 == 0);
    CHECK_FALSE(frame.resize(FrameSize{32, 32}));
    CHECK_FALSE(frame.resize(FrameSize{64, 32}));
    CHECK(frame.resize(FrameSize{64, 64}));
    CHECK_FALSE(frame.resize(FrameSize{0, 10}));
    CHECK(frame.size() == FrameSize{64, 64});
}

TEST_CASE("media/frame_queue: frames come out in order, and peek reads their timing", "[media][frame_queue]") {
    const auto pool = FramePool::create(4);
    FrameQueue queue(3);
    CHECK(queue.capacity() == 3);
    CHECK(queue.empty());
    CHECK_FALSE(queue.peek(0));
    for (int index = 0; index < 3; ++index) {
        REQUIRE(queue.push(stamped(*pool, index * 0.1), std::stop_token{}));
    }
    CHECK(queue.size() == 3);
    REQUIRE(queue.peek(0));
    CHECK(queue.peek(0)->ptsSeconds == 0.0);
    CHECK(queue.peek(2)->ptsSeconds == 0.2);
    CHECK_FALSE(queue.peek(3));
    for (int index = 0; index < 3; ++index) {
        const PooledFrame frame = queue.pop();
        REQUIRE(frame);
        CHECK(frame->timing.ptsSeconds == index * 0.1);
    }
    CHECK_FALSE(queue.pop());
    CHECK(pool->available() == 4);
}

TEST_CASE("media/frame_queue: a full queue holds the producer back until the consumer pops", "[media][frame_queue]") {
    const auto pool = FramePool::create(8);
    FrameQueue queue(2);
    std::atomic<int> pushed{0};
    std::jthread producer([&](std::stop_token stop) {
        for (int index = 0; index < 5; ++index) {
            if (!queue.push(stamped(*pool, index), stop)) {
                return;
            }
            ++pushed;
        }
    });
    REQUIRE(rv::test::waitUntil([&] { return pushed.load() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(pushed.load() == 2); // the third push waits for room
    CHECK(queue.size() == 2);

    std::vector<double> popped;
    REQUIRE(rv::test::waitUntil([&] {
        if (PooledFrame frame = queue.pop()) {
            popped.push_back(frame->timing.ptsSeconds);
        }
        return popped.size() == 5;
    }));
    CHECK(popped == std::vector<double>{0.0, 1.0, 2.0, 3.0, 4.0});
    producer.join();
    CHECK(pool->available() == 8);
}

TEST_CASE("media/frame_queue: a waiting producer leaves on stop or close, its frame back in the pool",
          "[media][frame_queue]") {
    const auto pool = FramePool::create(4);
    FrameQueue queue(1);
    REQUIRE(queue.push(stamped(*pool, 0.0), std::stop_token{}));

    std::atomic<int> result{-1};
    std::jthread producer([&](std::stop_token stop) { result = queue.push(stamped(*pool, 1.0), stop) ? 1 : 0; });
    REQUIRE(rv::test::waitUntil([&] { return pool->available() == 2; })); // the producer holds its frame
    producer.request_stop();
    producer.join();
    CHECK(result.load() == 0);
    CHECK(pool->available() == 3);

    std::jthread closing([&](std::stop_token stop) { result = queue.push(stamped(*pool, 2.0), stop) ? 1 : 0; });
    REQUIRE(rv::test::waitUntil([&] { return pool->available() == 2; }));
    queue.close();
    closing.join();
    CHECK(result.load() == 0);
    CHECK(queue.closed());
    CHECK(queue.size() == 1); // queued frames can still be popped
    CHECK(queue.pop());
    CHECK(pool->available() == 4);
}

TEST_CASE("media/frame_queue: clear returns every frame to the pool", "[media][frame_queue]") {
    const auto pool = FramePool::create(3);
    FrameQueue queue(3);
    for (int index = 0; index < 3; ++index) {
        REQUIRE(queue.push(stamped(*pool, index), std::stop_token{}));
    }
    CHECK(pool->available() == 0);
    queue.clear();
    CHECK(queue.empty());
    CHECK(pool->available() == 3);
    CHECK_FALSE(queue.push(PooledFrame{}, std::stop_token{})); // an empty handle is refused
}
