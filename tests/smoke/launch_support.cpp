#include "launch_support.hpp"

#include <algorithm>
#include <cwctype>
#include <system_error>
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/launcher/command_line.hpp>
#include <rv/launcher/environment.hpp>
#include <rv/platform_win/win32_process_runner.hpp>
#include <rv/platform_win/win32_util.hpp>

namespace rv::smoke {
namespace {

using Json = nlohmann::json;
using platform_win::lastErrorMessage;
using platform_win::UniqueHandle;

/// The window title retroverse.exe starts with (AppHost: "<name> <version>", then the frame rate).
constexpr std::wstring_view kTitlePrefix = L"Retroverse ";

[[nodiscard]] bool equalsIgnoreCase(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
               return std::towlower(x) == std::towlower(y);
           });
}

/// The executable's file name of an open process, or empty.
[[nodiscard]] std::wstring imageName(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
        return {};
    }
    path.resize(size);
    return std::filesystem::path(path).filename().wstring();
}

struct WindowSearch {
    DWORD processId = 0;
    HWND found = nullptr;
};

BOOL CALLBACK findWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search->processId) {
        return TRUE;
    }
    wchar_t title[256] = {};
    const int length = GetWindowTextW(window, title, 256);
    if (length > 0 && std::wstring_view(title, static_cast<std::size_t>(length)).starts_with(kTitlePrefix)) {
        search->found = window;
        return FALSE;
    }
    return TRUE;
}

} // namespace

std::filesystem::path fakeEmulatorExe() {
    return retroverseExe().parent_path() / "rv_fake_emulator.exe";
}

std::filesystem::path inputScript(std::string_view name) {
    return sourceDir() / "tests" / "input_scripts" / core::pathFromUtf8(name);
}

std::filesystem::path writeFakeCatalog(const std::filesystem::path& caseDir,
                                       const std::vector<std::string_view>& gameIds) {
    const std::filesystem::path fixture = sourceDir() / "tests" / "smoke" / "catalogs" / "fake_emulator_games.json";
    auto document = core::readJsonFile(fixture);
    INFO("fixture " << core::pathToUtf8(fixture));
    REQUIRE(document.has_value());
    const std::filesystem::path copied = caseDir / "copied_emulator.exe";
    const std::pair<std::string_view, std::filesystem::path> executables[] = {
        {"@FAKE_EMULATOR@", fakeEmulatorExe()},
        {"@GUARD_COPY@", copied},
        {"@MISSING_EMULATOR@", caseDir / "no_such_emulator.exe"},
    };
    for (Json& profile : (*document)["profiles"]) {
        const std::string placeholder = profile["executable"].get<std::string>();
        for (const auto& [name, path] : executables) {
            if (placeholder == name) {
                profile["executable"] = core::pathToUtf8(path.lexically_normal());
            }
        }
    }
    Json games = Json::array();
    for (const std::string_view id : gameIds) {
        const auto found = std::find_if((*document)["games"].begin(), (*document)["games"].end(),
                                        [&](const Json& game) { return game["id"].get<std::string>() == id; });
        INFO("game " << id);
        REQUIRE(found != (*document)["games"].end());
        games.push_back(*found);
        if (id == fake_games::kNotListed) {
            std::error_code error;
            std::filesystem::copy_file(fakeEmulatorExe(), copied, std::filesystem::copy_options::overwrite_existing,
                                       error);
            REQUIRE_FALSE(error);
        }
        if (found->contains("launch") && id != fake_games::kRomMissing) {
            const std::filesystem::path rom = caseDir / core::pathFromUtf8((*found)["launch"]["rom"].get<std::string>());
            REQUIRE(core::writeTextFileAtomic(rom, "ROM di prova\n").has_value());
        }
    }
    (*document)["games"] = std::move(games);
    const std::filesystem::path catalog = caseDir / "catalog.json";
    REQUIRE(core::writeTextFileAtomic(catalog, document->dump(2) + "\n").has_value());
    return catalog;
}

void ensureLaunchAllowList() {
    if (GetEnvironmentVariableW(L"RV_LAUNCH_ALLOWLIST", nullptr, 0) == 0 &&
        GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
        const std::wstring fake = fakeEmulatorExe().lexically_normal().wstring();
        REQUIRE(SetEnvironmentVariableW(L"RV_LAUNCH_ALLOWLIST", fake.c_str()));
    }
}

bool processRunning(std::uint32_t processId, std::string_view image) {
    const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId));
    if (!process) {
        return false;
    }
    if (WaitForSingleObject(process.get(), 0) != WAIT_TIMEOUT) {
        return false;
    }
    const auto expected = core::utf8ToWide(image);
    return expected && equalsIgnoreCase(imageName(process.get()), *expected);
}

std::optional<std::uint32_t> waitForExit(std::uint32_t processId, std::chrono::milliseconds timeout) {
    const UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, processId));
    if (!process) {
        return std::nullopt;
    }
    if (WaitForSingleObject(process.get(), static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
        return std::nullopt;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process.get(), &code)) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(code);
}

bool waitForFile(const std::filesystem::path& file, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::error_code error;
    while (!std::filesystem::exists(file, error)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

StartedApp::StartedApp(const std::filesystem::path& caseDir, std::string_view name, std::vector<std::string> arguments,
                       bool visible) {
    m_run.name = std::string(name);
    m_run.outputFile = caseDir / core::pathFromUtf8(m_run.name + ".log");
    m_run.reportFile = caseDir / core::pathFromUtf8(m_run.name + ".json");
    if (!visible) {
        arguments.emplace_back("--hidden");
    }
    arguments.insert(arguments.end(), {"--data-dir", arg(caseDir / "data"), "--run-report", arg(m_run.reportFile)});

    const std::string program = core::pathToUtf8(retroverseExe());
    const auto commandLine = launcher::buildCommandLine(program, arguments);
    REQUIRE(commandLine.has_value());
    auto wideCommandLine = core::utf8ToWide(*commandLine);
    REQUIRE(wideCommandLine.has_value());
    // Each configuration reads its own assets, as runCommand() runs them.
    std::wstring environment;
    const std::vector<launcher::EnvironmentOverride> overrides{{"RV_ASSET_ROOT", std::nullopt}};
    for (const launcher::EnvironmentVariable& variable :
         launcher::mergeEnvironment(platform_win::currentEnvironment(), overrides)) {
        const auto entry = core::utf8ToWide(variable.name + "=" + variable.value);
        REQUIRE(entry.has_value());
        environment += *entry;
        environment.push_back(L'\0');
    }
    environment.push_back(L'\0');

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE output = CreateFileW(m_run.outputFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    INFO(lastErrorMessage());
    REQUIRE(output != INVALID_HANDLE_VALUE);
    m_output = output;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = output;
    startup.hStdError = output;
    PROCESS_INFORMATION info{};
    const BOOL started = CreateProcessW(retroverseExe().c_str(), wideCommandLine->data(), nullptr, nullptr, TRUE,
                                       CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment.data(),
                                       caseDir.c_str(), &startup, &info);
    INFO("cannot start " << program << ": " << lastErrorMessage());
    REQUIRE(started);
    CloseHandle(info.hThread);
    m_process = info.hProcess;
    m_processId = info.dwProcessId;
}

StartedApp::~StartedApp() {
    if (m_process != nullptr) {
        if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(m_process, 1); // the harness ends what it started (never the product)
            WaitForSingleObject(m_process, INFINITE);
        }
        CloseHandle(m_process);
    }
    if (m_output != nullptr) {
        CloseHandle(m_output);
    }
}

bool StartedApp::closeWindow() {
    WindowSearch search;
    search.processId = m_processId;
    EnumWindows(findWindow, reinterpret_cast<LPARAM>(&search));
    return search.found != nullptr && PostMessageW(search.found, WM_CLOSE, 0, 0) != FALSE;
}

AppRun StartedApp::finish() {
    const DWORD waited =
        WaitForSingleObject(m_process, static_cast<DWORD>(std::chrono::milliseconds(kRunTimeout).count()));
    m_run.timedOut = waited == WAIT_TIMEOUT;
    if (m_run.timedOut) {
        TerminateProcess(m_process, 1);
        WaitForSingleObject(m_process, INFINITE);
    }
    DWORD code = 0;
    GetExitCodeProcess(m_process, &code);
    m_run.exitCode = static_cast<int>(code);
    CloseHandle(m_process);
    m_process = nullptr;
    CloseHandle(m_output);
    m_output = nullptr;
    if (auto report = app::readRunReport(m_run.reportFile)) {
        m_run.report = std::move(*report);
    } else {
        m_run.reportError = report.error().toString();
    }
    INFO(m_run.describe());
    REQUIRE_FALSE(m_run.timedOut);
    checkAudioOutput(m_run, true);
    return m_run;
}

} // namespace rv::smoke
