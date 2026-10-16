// The child runner of the tests (tests/support/child_process.hpp) on the fake emulator: arguments reach the program
// unchanged (quoted by rv::launcher::buildCommandLine), standard output and standard error come back apart or in one
// file, the environment changes, the exit code, and a program that outlives its timeout is terminated.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

#include <nlohmann/json.hpp>

#include <rv/core/json.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

#include "support/child_process.hpp"
#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
using rv::test::ChildRequest;
using rv::test::runChild;

namespace {

fs::path fakeEmulator() {
    return rv::core::executablePath().parent_path() / "rv_fake_emulator.exe";
}

ChildRequest fakeRequest(const rv::test::TempDir& dir, std::vector<std::string> arguments) {
    if (!fs::exists(fakeEmulator())) {
        SKIP("rv_fake_emulator.exe is not next to the tests");
    }
    ChildRequest request;
    request.executable = fakeEmulator();
    request.arguments = std::move(arguments);
    request.workingDirectory = dir.path();
    request.stdoutFile = dir.file("stdout.txt");
    request.stderrFile = dir.file("stderr.txt");
    request.timeout = std::chrono::seconds(60);
    return request;
}

} // namespace

TEST_CASE("support/child_process: arguments, standard output and standard error apart", "[support][child_process]") {
    rv::test::TempDir dir;
    const fs::path printed = dir.file("args.json");
    const std::vector<std::string> payload{"plain",
                                           "C:\\Program Files\\Retroverse\\scene.json",
                                           "",
                                           "with \"quotes\" inside",
                                           "trailing backslash\\",
                                           "C:\\dir with space\\",
                                           "back\\\\slashes\\\" quote",
                                           "tab\there",
                                           "100% Star (Europe).cue",
                                           "Pok\xC3\xA9mon \xF0\x9F\x8E\xAE"};
    std::vector<std::string> arguments{"--print-args", rv::core::pathToUtf8(printed), "--out", "to stdout \xC3\xA8",
                                       "--err", "to stderr", "--"};
    arguments.insert(arguments.end(), payload.begin(), payload.end());
    const auto result = runChild(fakeRequest(dir, arguments));
    REQUIRE(result.has_value());
    CHECK_FALSE(result->timedOut);
    CHECK(result->exitCode == 0);
    CHECK(result->standardOutput.find("to stdout \xC3\xA8") != std::string::npos);
    CHECK(result->standardOutput.find("to stderr") == std::string::npos);
    CHECK(result->standardError.find("to stderr") != std::string::npos);
    CHECK(result->standardError.find("to stdout") == std::string::npos);

    const auto args = rv::core::readJsonFile(printed);
    REQUIRE(args.has_value());
    CHECK((*args)["payload"].get<std::vector<std::string>>() == payload);
}

TEST_CASE("support/child_process: both streams in one file", "[support][child_process]") {
    rv::test::TempDir dir;
    ChildRequest request = fakeRequest(dir, {"--out", "first", "--err", "second"});
    request.stderrFile = request.stdoutFile;
    const auto result = runChild(request);
    REQUIRE(result.has_value());
    CHECK(result->standardOutput.find("first") != std::string::npos);
    CHECK(result->standardOutput.find("second") != std::string::npos);
    CHECK(result->standardError == result->standardOutput);
}

TEST_CASE("support/child_process: exit codes and the environment", "[support][child_process]") {
    rv::test::TempDir dir;
    const fs::path printed = dir.file("env.json");
    REQUIRE(SetEnvironmentVariableW(L"RV_FAKE_INHERITED", L"inherited"));
    REQUIRE(SetEnvironmentVariableW(L"RV_FAKE_REMOVED", L"removed"));
    ChildRequest request = fakeRequest(dir, {"--exit-code", "7", "--print-env", rv::core::pathToUtf8(printed)});
    request.environment = {{"RV_FAKE_SET", std::string("set \xC3\xA9")}, {"rv_fake_removed", std::nullopt}};
    const auto result = runChild(request);
    SetEnvironmentVariableW(L"RV_FAKE_INHERITED", nullptr);
    SetEnvironmentVariableW(L"RV_FAKE_REMOVED", nullptr);
    REQUIRE(result.has_value());
    CHECK(result->exitCode == 7);
    const auto variables = rv::core::readJsonFile(printed);
    REQUIRE(variables.has_value());
    CHECK((*variables)["RV_FAKE_SET"] == "set \xC3\xA9");
    CHECK((*variables)["RV_FAKE_INHERITED"] == "inherited");
    CHECK_FALSE(variables->contains("RV_FAKE_REMOVED"));
}

TEST_CASE("support/child_process: a program that outlives its timeout is terminated", "[support][child_process]") {
    rv::test::TempDir dir;
    // A hang that would end by itself after a minute (the fake's exit timeout) should this test die before its end.
    ChildRequest request = fakeRequest(dir, {"--hang", "--exit-timeout-ms", "60000"});
    request.timeout = std::chrono::milliseconds(500);
    const auto result = runChild(request);
    REQUIRE(result.has_value());
    CHECK(result->timedOut);
    CHECK(result->elapsed < std::chrono::seconds(10));
}

TEST_CASE("support/child_process: the fake emulator's exit timeout ends it by itself", "[support][child_process]") {
    rv::test::TempDir dir;
    const auto result = runChild(fakeRequest(dir, {"--hang", "--exit-timeout-ms", "200"}));
    REQUIRE(result.has_value());
    CHECK_FALSE(result->timedOut);
    CHECK(result->exitCode == 124);
}

TEST_CASE("support/child_process: what cannot run is an error", "[support][child_process]") {
    rv::test::TempDir dir;
    const auto errorOf = [](const ChildRequest& request) {
        const auto result = runChild(request);
        REQUIRE_FALSE(result.has_value());
        return result.error().code;
    };
    ChildRequest request = fakeRequest(dir, {"--exit-code", "0"});
    request.arguments.push_back(std::string("nul\0inside", 10));
    CHECK(errorOf(request) == rv::ErrorCode::ValidationFailed);

    request = fakeRequest(dir, {});
    request.stdoutFile.clear();
    CHECK(errorOf(request) == rv::ErrorCode::InvalidArgument);

    request = fakeRequest(dir, {});
    request.executable = dir.file("missing.exe");
    CHECK(errorOf(request) == rv::ErrorCode::PlatformError);

    request = fakeRequest(dir, {});
    request.stdoutFile = dir.file("no such folder/stdout.txt");
    CHECK(errorOf(request) == rv::ErrorCode::IoError);
}
