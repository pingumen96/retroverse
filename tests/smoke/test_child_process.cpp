// The child processes of the smoke tests: arguments reach them unchanged (paths with spaces, quotes, trailing
// backslashes), and a process that does not end in time is terminated.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <windows.h>

#include <shellapi.h>

#include "child_process.hpp"
#include "smoke_support.hpp"

using rv::smoke::quoteArgument;

namespace {

/// What CommandLineToArgvW, the parser of the C runtime and SDL, reads back from `arguments` quoted after a program.
std::vector<std::wstring> parsedBack(const std::vector<std::wstring>& arguments) {
    std::wstring commandLine = L"program.exe";
    for (const std::wstring& argument : arguments) {
        commandLine += L' ';
        commandLine += quoteArgument(argument);
    }
    int count = 0;
    wchar_t** parsed = CommandLineToArgvW(commandLine.c_str(), &count);
    std::vector<std::wstring> result;
    for (int i = 1; parsed != nullptr && i < count; ++i) {
        result.emplace_back(parsed[i]);
    }
    LocalFree(parsed);
    return result;
}

} // namespace

TEST_CASE("support/child_process: quoted arguments read back unchanged", "[gpu][support]") {
    const std::vector<std::wstring> arguments{L"--scene",
                                              L"plain",
                                              L"C:\\Program Files\\Retroverse\\scene.json",
                                              L"",
                                              L"with \"quotes\" inside",
                                              L"trailing backslash\\",
                                              L"C:\\dir with space\\",
                                              L"back\\\\slashes\\\" quote",
                                              L"tab\there",
                                              L"0,0,3.2,0,-6"};
    CHECK(parsedBack(arguments) == arguments);
    CHECK(quoteArgument(L"plain") == L"plain");
    CHECK(quoteArgument(L"") == L"\"\"");
}

TEST_CASE("support/child_process: a process that outlives its timeout is terminated", "[gpu][support]") {
    const std::filesystem::path dir = rv::smoke::freshCaseDir("support_child_process");
    wchar_t system[MAX_PATH]{};
    REQUIRE(GetSystemDirectoryW(system, MAX_PATH) > 0);
    rv::smoke::ProcessRequest request;
    // Ten seconds of pings to the loopback address, one second apart: one process, nothing left behind once terminated.
    request.executable = std::filesystem::path(system) / L"PING.EXE";
    request.arguments = {"-n", "11", "127.0.0.1"};
    request.workingDirectory = dir;
    request.outputFile = dir / "timeout.log";
    request.timeout = std::chrono::milliseconds(500);
    const auto outcome = rv::smoke::runProcess(request);
    REQUIRE(outcome.has_value());
    CHECK(outcome->timedOut);
    CHECK(outcome->elapsed < std::chrono::seconds(5));

    request.executable = std::filesystem::path(system) / L"cmd.exe";
    request.arguments = {"/c", "exit 7"};
    request.outputFile = dir / "exit.log";
    request.timeout = std::chrono::seconds(30);
    const auto exited = rv::smoke::runProcess(request);
    REQUIRE(exited.has_value());
    CHECK_FALSE(exited->timedOut);
    CHECK(exited->exitCode == 7);
}
