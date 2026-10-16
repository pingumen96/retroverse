#include "allocation_counter.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <span>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rv::spike_ui {
namespace {

/// The modules whose import tables are patched, in AllocationCounts order.
enum ModuleIndex : std::size_t { kRetroverse = 0, kRmlUi = 1, kModuleCount = 2 };

/// The CRT allocation functions counted.
enum FunctionIndex : std::size_t {
    kMalloc,
    kCalloc,
    kRealloc,
    kRecalloc,
    kAlignedMalloc,
    kAlignedRealloc,
    kMallocDbg,
    kCallocDbg,
    kReallocDbg,
    kFunctionCount
};

using MallocFn = void*(__cdecl*)(std::size_t);
using CallocFn = void*(__cdecl*)(std::size_t, std::size_t);
using ReallocFn = void*(__cdecl*)(void*, std::size_t);
using RecallocFn = void*(__cdecl*)(void*, std::size_t, std::size_t);
using AlignedMallocFn = void*(__cdecl*)(std::size_t, std::size_t);
using AlignedReallocFn = void*(__cdecl*)(void*, std::size_t, std::size_t);
using MallocDbgFn = void*(__cdecl*)(std::size_t, int, const char*, int);
using CallocDbgFn = void*(__cdecl*)(std::size_t, std::size_t, int, const char*, int);
using ReallocDbgFn = void*(__cdecl*)(void*, std::size_t, int, const char*, int);

/// What each module's import table pointed at before the counting functions replaced it: the CRT's functions. Written
/// before the table entry is patched, read by the counting functions.
std::array<std::array<void*, kFunctionCount>, kModuleCount> g_originals{};
/// Allocations counted per module. Only the counting thread (the main thread) writes them: counting is thread-local.
std::array<std::int64_t, kModuleCount> g_counts{};
thread_local bool t_counting = false;
std::atomic<bool> g_installed{false};

template <typename Fn>
[[nodiscard]] Fn original(std::size_t module, FunctionIndex function) noexcept {
    return reinterpret_cast<Fn>(g_originals[module][function]);
}

/// The counting functions of module M.
template <std::size_t M>
struct Counting {
    static void counted() noexcept {
        if (t_counting) {
            ++g_counts[M];
        }
    }
    static void* __cdecl malloc(std::size_t size) {
        counted();
        return original<MallocFn>(M, kMalloc)(size);
    }
    static void* __cdecl calloc(std::size_t number, std::size_t size) {
        counted();
        return original<CallocFn>(M, kCalloc)(number, size);
    }
    static void* __cdecl realloc(void* block, std::size_t size) {
        if (size != 0) {
            counted();
        }
        return original<ReallocFn>(M, kRealloc)(block, size);
    }
    static void* __cdecl recalloc(void* block, std::size_t number, std::size_t size) {
        if (number != 0 && size != 0) {
            counted();
        }
        return original<RecallocFn>(M, kRecalloc)(block, number, size);
    }
    static void* __cdecl alignedMalloc(std::size_t size, std::size_t alignment) {
        counted();
        return original<AlignedMallocFn>(M, kAlignedMalloc)(size, alignment);
    }
    static void* __cdecl alignedRealloc(void* block, std::size_t size, std::size_t alignment) {
        if (size != 0) {
            counted();
        }
        return original<AlignedReallocFn>(M, kAlignedRealloc)(block, size, alignment);
    }
    static void* __cdecl mallocDbg(std::size_t size, int type, const char* file, int line) {
        counted();
        return original<MallocDbgFn>(M, kMallocDbg)(size, type, file, line);
    }
    static void* __cdecl callocDbg(std::size_t number, std::size_t size, int type, const char* file, int line) {
        counted();
        return original<CallocDbgFn>(M, kCallocDbg)(number, size, type, file, line);
    }
    static void* __cdecl reallocDbg(void* block, std::size_t size, int type, const char* file, int line) {
        if (size != 0) {
            counted();
        }
        return original<ReallocDbgFn>(M, kReallocDbg)(block, size, type, file, line);
    }
};

/// An import replaced: the imported name and the counting function that takes its place.
struct Hook {
    const char* name = nullptr;
    FunctionIndex function = kMalloc;
    void* replacement = nullptr;
};

template <std::size_t M>
[[nodiscard]] std::array<Hook, kFunctionCount> hooksOf() {
    using C = Counting<M>;
    return {{
        {"malloc", kMalloc, reinterpret_cast<void*>(&C::malloc)},
        {"calloc", kCalloc, reinterpret_cast<void*>(&C::calloc)},
        {"realloc", kRealloc, reinterpret_cast<void*>(&C::realloc)},
        {"_recalloc", kRecalloc, reinterpret_cast<void*>(&C::recalloc)},
        {"_aligned_malloc", kAlignedMalloc, reinterpret_cast<void*>(&C::alignedMalloc)},
        {"_aligned_realloc", kAlignedRealloc, reinterpret_cast<void*>(&C::alignedRealloc)},
        {"_malloc_dbg", kMallocDbg, reinterpret_cast<void*>(&C::mallocDbg)},
        {"_calloc_dbg", kCallocDbg, reinterpret_cast<void*>(&C::callocDbg)},
        {"_realloc_dbg", kReallocDbg, reinterpret_cast<void*>(&C::reallocDbg)},
    }};
}

#if defined(_WIN32)

/// Writes `value` into the import table entry `slot`, which the loader may have left read-only.
[[nodiscard]] bool writeSlot(void** slot, void* value) noexcept {
    DWORD protection = 0;
    if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection) == 0) {
        return false;
    }
    *slot = value;
    DWORD unused = 0;
    VirtualProtect(slot, sizeof(void*), protection, &unused);
    return true;
}

/// Points the entries of `module`'s import table that import one of `hooks` by name at the counting functions,
/// appending each change to `patches`. The original goes into g_originals before the entry changes, so a call on
/// another thread finds either the CRT's function or a counting function that can call it.
template <typename Patch>
void patchImports(HMODULE module, std::size_t moduleIndex, std::span<const Hook> hooks, std::vector<Patch>& patches) {
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        return;
    }
    for (const auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
         descriptor->Name != 0; ++descriptor) {
        if (descriptor->OriginalFirstThunk == 0 || descriptor->FirstThunk == 0) {
            continue; // no name table: nothing to match
        }
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            const char* name = reinterpret_cast<const char*>(byName->Name);
            for (const Hook& hook : hooks) {
                if (std::strcmp(name, hook.name) != 0) {
                    continue;
                }
                void** slot = reinterpret_cast<void**>(&slots->u1.Function);
                void* const crt = *slot;
                void*& stored = g_originals[moduleIndex][hook.function];
                if (crt == nullptr || (stored != nullptr && stored != crt)) {
                    break; // unresolved, or two different functions behind one name: left alone
                }
                stored = crt;
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (writeSlot(slot, hook.replacement)) {
                    patches.push_back(Patch{slot, crt});
                }
                break;
            }
        }
    }
}

#endif

} // namespace

Expected<std::unique_ptr<AllocationCounter>> AllocationCounter::install() {
#if defined(_WIN32)
    if (g_installed.exchange(true)) {
        return makeError(ErrorCode::InvalidArgument, "an allocation counter is installed already");
    }
    const HMODULE executable = GetModuleHandleW(nullptr);
    const HMODULE rmlui = GetModuleHandleW(L"rmlui.dll");
    if (executable == nullptr || rmlui == nullptr) {
        g_installed.store(false);
        return makeError(ErrorCode::PlatformError, "the spike's executable or rmlui.dll is not loaded");
    }
    std::vector<Patch> patches;
    const std::array<Hook, kFunctionCount> ours = hooksOf<kRetroverse>();
    const std::array<Hook, kFunctionCount> theirs = hooksOf<kRmlUi>();
    patchImports(executable, kRetroverse, ours, patches);
    const std::size_t executablePatches = patches.size();
    patchImports(rmlui, kRmlUi, theirs, patches);
    if (executablePatches == 0 || patches.size() == executablePatches) {
        restore(patches);
        g_installed.store(false);
        return makeError(ErrorCode::PlatformError,
                         "no allocation function found in the import tables of spike_ui.exe and rmlui.dll",
                         std::to_string(executablePatches) + " + " + std::to_string(patches.size() - executablePatches));
    }
    g_counts = {};
    return std::unique_ptr<AllocationCounter>(new AllocationCounter(std::move(patches)));
#else
    return makeError(ErrorCode::Unsupported, "allocation counting patches Windows import tables");
#endif
}

AllocationCounter::~AllocationCounter() {
    restore(m_patches);
    g_installed.store(false);
}

void AllocationCounter::restore(const std::vector<Patch>& patches) noexcept {
#if defined(_WIN32)
    for (const Patch& patch : patches) {
        static_cast<void>(writeSlot(patch.slot, patch.original));
    }
#else
    static_cast<void>(patches);
#endif
}

void AllocationCounter::begin() noexcept {
    t_counting = true;
}

void AllocationCounter::end() noexcept {
    t_counting = false;
}

AllocationCounts AllocationCounter::take() noexcept {
    AllocationCounts counts;
    counts.retroverse = std::exchange(g_counts[kRetroverse], 0);
    counts.rmlui = std::exchange(g_counts[kRmlUi], 0);
    return counts;
}

} // namespace rv::spike_ui
