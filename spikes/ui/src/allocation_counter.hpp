#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rv/core/expected.hpp>

namespace rv::spike_ui {

/// Heap allocations counted during a span of the main thread, by the module that asked for them; -1 when not counted.
struct AllocationCounts {
    /// spike_ui.exe: Retroverse's code (rv_ui, rv_ui_model, the spike) and the vendored RmlUi backends built into it.
    std::int64_t retroverse = -1;
    /// rmlui.dll: RmlUi itself.
    std::int64_t rmlui = -1;

    [[nodiscard]] bool measured() const noexcept { return retroverse >= 0 && rmlui >= 0; }
};

/// Counts the heap allocations that spike_ui.exe and rmlui.dll make on the counting thread between begin() and end(),
/// in Release as in Debug builds: the entries of their import address tables for the CRT's allocation functions
/// (malloc, calloc, realloc, _recalloc, _aligned_malloc, _aligned_realloc and the debug CRT's _malloc_dbg, _calloc_dbg,
/// _realloc_dbg; operator new calls one of them) are pointed at counting functions that call the CRT's, and restored by
/// the destructor. Frees are not counted, nor a realloc to size 0. Other modules (the OpenGL driver, SDL, FreeType,
/// spdlog) are not seen, nor allocations of other threads. Debug counts include the debug iterators' container proxies
/// (one per std::vector or std::string built, empty ones too), so only Release counts tell what a release build does.
/// One counter at a time; install and destroy it on the main thread. Windows only.
class AllocationCounter {
public:
    /// Patches the import tables. Errors: InvalidArgument when a counter is installed already, PlatformError when a
    /// module or its allocation imports cannot be found or patched (nothing is left patched), Unsupported off Windows.
    [[nodiscard]] static Expected<std::unique_ptr<AllocationCounter>> install();
    ~AllocationCounter();
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;

    /// Starts and stops counting the allocations of the calling thread (whether or not a counter is installed).
    static void begin() noexcept;
    static void end() noexcept;
    /// The allocations counted since the last take(); the counts start again from zero.
    [[nodiscard]] AllocationCounts take() noexcept;
    /// Import table entries patched, per module (logged at start).
    [[nodiscard]] std::size_t patchedEntries() const noexcept { return m_patches.size(); }

private:
    struct Patch {
        void** slot = nullptr;
        void* original = nullptr;
    };

    explicit AllocationCounter(std::vector<Patch> patches) noexcept : m_patches(std::move(patches)) {}
    static void restore(const std::vector<Patch>& patches) noexcept;

    std::vector<Patch> m_patches;
};

} // namespace rv::spike_ui
