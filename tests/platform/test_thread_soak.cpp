// Threads started and joined in a process with an OpenGL context (I5 step 7's review: the soak's growth per clip
// opened, each clip on a thread of its own). Hidden ([.soak]) and informative: it prints the private bytes a thread
// costs once joined, without a GL context, then with a hidden window's core 4.5 context current on the main thread
// (RV_SOAK_GL_DEBUG=1 asks for a debug context), idle and then presenting frames while the threads come and go, as the
// app's render loop does while its decoders' threads do. Run it by hand, in Release:
//   build/msvc-x64-release/bin/rv_platform_tests.exe "[soak]"
// RV_SOAK_THREADS sets the threads per measurement (2000 by default).

#include <rv/platform/graphics_window.hpp>
#include <rv/platform/sdl_platform.hpp>

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <psapi.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::uint64_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters));
    return counters.PrivateUsage;
}

int envInt(const char* name, int fallback) {
    const char* text = std::getenv(name);
    return text != nullptr && std::atoi(text) > 0 ? std::atoi(text) : fallback;
}

/// What a decoder's thread does besides decoding: names itself, allocates blocks of a frame's sizes and frees them,
/// for about `milliseconds`.
void threadBody(int milliseconds) {
    SetThreadDescription(GetCurrentThread(), L"rv soak thread");
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        std::vector<std::unique_ptr<std::uint8_t[]>> blocks;
        for (const std::size_t size :
             {std::size_t{307200}, std::size_t{76800}, std::size_t{76800}, std::size_t{4096}}) {
            blocks.push_back(std::make_unique<std::uint8_t[]>(size));
            std::memset(blocks.back().get(), 1, size);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < until);
}

/// Private bytes per thread started and joined, after a warm-up; `frame` runs on this thread while each one runs.
double bytesPerThread(int threads, int milliseconds, const std::function<void()>& frame) {
    const auto once = [&] {
        std::thread thread(threadBody, milliseconds);
        if (frame) {
            for (int index = 0; index < 4; ++index) {
                frame();
            }
        }
        thread.join();
    };
    for (int index = 0; index < 50; ++index) {
        once();
    }
    const std::uint64_t before = privateBytes();
    for (int index = 0; index < threads; ++index) {
        once();
    }
    const std::uint64_t after = privateBytes();
    return (static_cast<double>(after) - static_cast<double>(before)) / threads;
}

} // namespace

TEST_CASE("soak: private bytes per thread started and joined, without and with a GL context", "[.soak]") {
    const int threads = envInt("RV_SOAK_THREADS", 2000);
    const bool debugContext = envInt("RV_SOAK_GL_DEBUG", 0) == 1;

    std::cout << "No GL context: " << threads << " threads, " << bytesPerThread(threads, 0, {}) << " bytes each\n";

    auto platform = rv::platform::SdlPlatform::init({"Retroverse thread soak", "0", "dev.retroverse.threadsoak"});
    REQUIRE(platform.has_value());
    rv::platform::WindowDesc window;
    window.hidden = true;
    window.width = 320;
    window.height = 240;
    rv::platform::GlContextDesc context;
    context.debug = debugContext;
    context.vsync = false;
    auto graphics = rv::platform::createGraphicsWindow(window, context);
    REQUIRE(graphics.has_value());
    const std::string kind = debugContext ? "Debug" : "Non-debug";

    std::cout << kind << " GL 4.5 context, idle: " << threads << " threads, " << bytesPerThread(threads, 0, {})
              << " bytes each\n";

    using ClearColor = void(APIENTRY*)(float, float, float, float);
    using Clear = void(APIENTRY*)(unsigned);
    const auto clearColor = reinterpret_cast<ClearColor>(graphics->glLoader()("glClearColor"));
    const auto clear = reinterpret_cast<Clear>(graphics->glLoader()("glClear"));
    REQUIRE(clearColor != nullptr);
    REQUIRE(clear != nullptr);
    int frames = 0;
    const auto frame = [&] {
        clearColor(static_cast<float>(frames % 7) / 7.0f, 0.2f, 0.3f, 1.0f);
        clear(0x00004000); // GL_COLOR_BUFFER_BIT
        static_cast<void>(graphics->swap());
        ++frames;
    };
    // The same frames without the threads: what presenting alone costs.
    for (int index = 0; index < 200; ++index) {
        frame();
    }
    const std::uint64_t before = privateBytes();
    for (int index = 0; index < threads; ++index) {
        for (int step = 0; step < 4; ++step) {
            frame();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cout << kind << " GL 4.5 context, presenting, no thread: "
              << (static_cast<double>(privateBytes()) - static_cast<double>(before)) / threads
              << " bytes per 4 frames and 20 ms\n";
    std::cout << kind << " GL 4.5 context, presenting: " << threads << " threads of 20 ms, "
              << bytesPerThread(threads, 20, frame) << " bytes each (" << frames << " frames)\n";

    // The same work on one thread that lives on, given a job at a time (the decoders' slots' workers).
    {
        std::mutex mutex;
        std::condition_variable wake;
        int jobs = 0;
        int done = 0;
        bool quit = false;
        std::thread worker([&] {
            std::unique_lock lock(mutex);
            for (;;) {
                wake.wait(lock, [&] { return quit || jobs > done; });
                if (quit) {
                    return;
                }
                lock.unlock();
                threadBody(20);
                lock.lock();
                ++done;
                wake.notify_all();
            }
        });
        const auto job = [&] {
            {
                const std::lock_guard lock(mutex);
                ++jobs;
            }
            wake.notify_all();
            for (int step = 0; step < 4; ++step) {
                frame();
            }
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return done == jobs; });
        };
        for (int index = 0; index < 50; ++index) {
            job();
        }
        const std::uint64_t start = privateBytes();
        for (int index = 0; index < threads; ++index) {
            job();
        }
        std::cout << kind << " GL 4.5 context, presenting: " << threads << " jobs of 20 ms on one thread, "
                  << (static_cast<double>(privateBytes()) - static_cast<double>(start)) / threads << " bytes each\n";
        {
            const std::lock_guard lock(mutex);
            quit = true;
        }
        wake.notify_all();
        worker.join();
    }
    SUCCEED();
}
