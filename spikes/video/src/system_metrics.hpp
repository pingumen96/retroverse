#pragma once

// Windows process and machine metrics of the video spike's report: memory (GetProcessMemoryInfo), CPU time of the
// machine, the process and the main thread (GetSystemTimes, GetProcessTimes, GetThreadTimes), the CPU's name.

#include <chrono>
#include <cstdint>
#include <string>

namespace rv::spike {

/// The process's memory, MiB.
struct MemorySample {
    double workingSetMiB = 0.0;
    double peakWorkingSetMiB = 0.0;
    double privateMiB = 0.0;
};

[[nodiscard]] MemorySample sampleMemory() noexcept;

/// CPU time counters at one moment.
struct CpuTimes {
    std::chrono::steady_clock::time_point at;
    double systemIdleSeconds = 0.0; ///< summed over every logical processor
    double systemBusySeconds = 0.0; ///< kernel (idle excluded) + user, summed over every logical processor
    double processSeconds = 0.0;    ///< this process, user + kernel
    double mainThreadSeconds = 0.0; ///< the calling thread, user + kernel
};

/// Samples the counters; call it on the main thread so that mainThreadSeconds is the main thread's.
[[nodiscard]] CpuTimes sampleCpu() noexcept;

/// Loads over the span between two samples, percent.
struct CpuLoad {
    double wallSeconds = 0.0;
    double machineBusyPercent = 0.0; ///< every program, of the whole machine
    double processPercent = 0.0;     ///< this process, of one logical processor (can exceed 100)
    double mainThreadPercent = 0.0;  ///< of one logical processor
};

[[nodiscard]] CpuLoad cpuLoadBetween(const CpuTimes& from, const CpuTimes& to) noexcept;

/// Sets the priority class of this process: "normal", "above-normal" or "high". False when Windows refuses it.
bool setProcessPriority(const std::string& priority) noexcept;

/// "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz", from the registry; empty when unknown.
[[nodiscard]] std::string cpuName();
[[nodiscard]] int logicalProcessorCount() noexcept;

} // namespace rv::spike
