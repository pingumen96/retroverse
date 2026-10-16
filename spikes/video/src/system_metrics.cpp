#include "system_metrics.hpp"

#include <algorithm>
#include <array>

#include <windows.h>
#include <psapi.h>

#include <rv/core/text.hpp>

namespace rv::spike {
namespace {

constexpr double kMiB = 1024.0 * 1024.0;

double seconds(const FILETIME& time) noexcept {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<double>(value.QuadPart) * 1e-7;
}

} // namespace

MemorySample sampleMemory() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    MemorySample sample;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) != 0) {
        sample.workingSetMiB = static_cast<double>(counters.WorkingSetSize) / kMiB;
        sample.peakWorkingSetMiB = static_cast<double>(counters.PeakWorkingSetSize) / kMiB;
        sample.privateMiB = static_cast<double>(counters.PrivateUsage) / kMiB;
    }
    return sample;
}

CpuTimes sampleCpu() noexcept {
    CpuTimes times;
    times.at = std::chrono::steady_clock::now();
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user) != 0) {
        times.systemIdleSeconds = seconds(idle);
        times.systemBusySeconds = seconds(kernel) - seconds(idle) + seconds(user); // kernel time includes idle time
    }
    FILETIME creation{};
    FILETIME exit{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) != 0) {
        times.processSeconds = seconds(kernel) + seconds(user);
    }
    if (GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user) != 0) {
        times.mainThreadSeconds = seconds(kernel) + seconds(user);
    }
    return times;
}

CpuLoad cpuLoadBetween(const CpuTimes& from, const CpuTimes& to) noexcept {
    CpuLoad load;
    load.wallSeconds = std::chrono::duration<double>(to.at - from.at).count();
    const double idle = to.systemIdleSeconds - from.systemIdleSeconds;
    const double busy = to.systemBusySeconds - from.systemBusySeconds;
    if (idle + busy > 0.0) {
        load.machineBusyPercent = 100.0 * busy / (idle + busy);
    }
    if (load.wallSeconds > 0.0) {
        load.processPercent = 100.0 * (to.processSeconds - from.processSeconds) / load.wallSeconds;
        load.mainThreadPercent = 100.0 * (to.mainThreadSeconds - from.mainThreadSeconds) / load.wallSeconds;
    }
    return load;
}

bool setProcessPriority(const std::string& priority) noexcept {
    DWORD priorityClass = NORMAL_PRIORITY_CLASS;
    if (priority == "above-normal") {
        priorityClass = ABOVE_NORMAL_PRIORITY_CLASS;
    } else if (priority == "high") {
        priorityClass = HIGH_PRIORITY_CLASS;
    }
    return SetPriorityClass(GetCurrentProcess(), priorityClass) != 0;
}

std::string cpuName() {
    std::array<wchar_t, 256> name{};
    DWORD bytes = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, name.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    return std::string(core::trim(core::pathToUtf8(std::filesystem::path(name.data()))));
}

int logicalProcessorCount() noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return std::max(1, static_cast<int>(info.dwNumberOfProcessors));
}

} // namespace rv::spike
