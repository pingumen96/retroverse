#include "harness.hpp"

#include <array>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <tlhelp32.h>

#include <SDL3/SDL_video.h>
#include <nlohmann/json.hpp>

#include <rv/core/text.hpp>

namespace rv::spikes::launch::harness {
namespace {

std::uint64_t ticks(const FILETIME& time) noexcept {
    return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

struct WindowQuery {
    std::string_view imageName;
    std::string_view titlePart;
    std::vector<void*>* found = nullptr;
};

BOOL CALLBACK collectWindow(HWND window, LPARAM parameter) {
    auto* query = reinterpret_cast<WindowQuery*>(parameter);
    if (!IsWindowVisible(window)) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (!core::equalsIgnoreCaseAscii(processImageName(processId), query->imageName)) {
        return TRUE;
    }
    std::array<wchar_t, 512> title{};
    const int length = GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    if (narrow(std::wstring_view(title.data(), static_cast<std::size_t>(length > 0 ? length : 0)))
            .find(query->titlePart) == std::string::npos) {
        return TRUE;
    }
    query->found->push_back(window);
    return TRUE;
}

nlohmann::json describeMode(const SDL_DisplayMode* mode) {
    if (mode == nullptr) {
        return nullptr;
    }
    return nlohmann::json{{"width", mode->w},
                          {"height", mode->h},
                          {"refreshRate", mode->refresh_rate},
                          {"pixelDensity", mode->pixel_density}};
}

} // namespace

void CpuLoadSampler::start() noexcept {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user)) {
        m_idle = ticks(idle);
        m_kernel = ticks(kernel);
        m_user = ticks(user);
    }
}

double CpuLoadSampler::load() const noexcept {
    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        return 0.0;
    }
    // Kernel time includes the idle time.
    const std::uint64_t idleDelta = ticks(idle) - m_idle;
    const std::uint64_t totalDelta = (ticks(kernel) - m_kernel) + (ticks(user) - m_user);
    if (totalDelta == 0 || idleDelta > totalDelta) {
        return 0.0;
    }
    return static_cast<double>(totalDelta - idleDelta) / static_cast<double>(totalDelta);
}

std::uint32_t foregroundLockTimeoutMs() {
    DWORD timeout = 0;
    if (!SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &timeout, 0)) {
        return 0;
    }
    return timeout;
}

std::uint32_t millisecondsSinceLastInput() {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info)) {
        return 0;
    }
    return GetTickCount() - info.dwTime;
}

bool terminateProcess(std::uint32_t processId, std::uint32_t exitCode) {
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        return false;
    }
    TerminateProcess(process, exitCode);
    const bool gone = WaitForSingleObject(process, 10000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return gone;
}

std::string processImageName(std::uint32_t processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return {};
    }
    std::array<wchar_t, 1024> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    std::string name;
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        const std::wstring_view path(buffer.data(), size);
        const std::size_t slash = path.find_last_of(L"\\/");
        name = narrow(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
    }
    CloseHandle(process);
    return name;
}

bool isProcessRunning(std::string_view imageName) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !running; more = Process32NextW(snapshot, &entry)) {
        running = core::equalsIgnoreCaseAscii(narrow(entry.szExeFile), imageName);
    }
    CloseHandle(snapshot);
    return running;
}

std::vector<void*> findWindows(std::string_view imageName, std::string_view titlePart) {
    std::vector<void*> found;
    WindowQuery query{imageName, titlePart, &found};
    EnumWindows(&collectWindow, reinterpret_cast<LPARAM>(&query));
    return found;
}

void closeWindow(void* window) {
    PostMessageW(static_cast<HWND>(window), WM_CLOSE, 0, 0);
}

bool windowExists(void* window) {
    return IsWindow(static_cast<HWND>(window)) != FALSE;
}

std::filesystem::path systemNotepad() {
    std::array<wchar_t, MAX_PATH> directory{};
    const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    return std::filesystem::path(std::wstring(directory.data(), length)) / L"notepad.exe";
}

nlohmann::json describeDisplays() {
    nlohmann::json displays = nlohmann::json::array();
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        SDL_Rect bounds{};
        SDL_GetDisplayBounds(ids[i], &bounds);
        const char* name = SDL_GetDisplayName(ids[i]);
        displays.push_back(nlohmann::json{{"id", ids[i]},
                                          {"name", name != nullptr ? name : ""},
                                          {"primary", ids[i] == SDL_GetPrimaryDisplay()},
                                          {"bounds", {bounds.x, bounds.y, bounds.w, bounds.h}},
                                          {"contentScale", SDL_GetDisplayContentScale(ids[i])},
                                          {"currentMode", describeMode(SDL_GetCurrentDisplayMode(ids[i]))},
                                          {"desktopMode", describeMode(SDL_GetDesktopDisplayMode(ids[i]))}});
    }
    SDL_free(ids);
    return displays;
}

} // namespace rv::spikes::launch::harness
